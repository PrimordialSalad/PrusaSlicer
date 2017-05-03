#include "../GCode.hpp"
#include "CoolingBuffer.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <iostream>

namespace Slic3r {

std::string CoolingBuffer::append(const std::string &gcode, size_t object_id, size_t layer_id, bool is_support)
{
    std::string out;
    size_t signature = object_id * 2 + is_support ? 1 : 0;
    if (m_object_ids_visited.find(signature) != m_object_ids_visited.end())
        // For a single print_z, a combination of (object_id, is_support) could repeat once only.
        // If the combination of (object_id, is_support) reappears, this must be for another print_z,
        // therefore a layer has to be finalized.
        out = this->flush();
 
    m_object_ids_visited.insert(signature);
    m_layer_id   = layer_id;
    m_gcode     += gcode;
    // This is a very rough estimate of the print time, 
    // not taking into account the acceleration curves generated by the printer firmware.
    m_elapsed_time += m_gcodegen.get_reset_elapsed_time();
    return out;
}

void apply_speed_factor(std::string &line, float speed_factor, float min_print_speed)
{
    // find pos of F
    size_t pos = line.find_first_of('F');
    size_t last_pos = line.find_first_of(' ', pos+1);
    
    // extract current speed
    float speed;
    {
        std::istringstream iss(line.substr(pos+1, last_pos));
        iss >> speed;
    }
    
    // change speed
    speed *= speed_factor;
    speed = std::max(speed, min_print_speed);
    
    // replace speed in string
    {
        std::ostringstream oss;
        oss << speed;
        line.replace(pos+1, (last_pos-pos), oss.str());
    }
}

std::string CoolingBuffer::flush()
{
    const FullPrintConfig &config = m_gcodegen.config();
    
    std::string gcode   = m_gcode;
    float elapsed       = m_elapsed_time;
    m_gcode.clear();
    m_elapsed_time = 0.;

    int fan_speed = config.fan_always_on ? config.min_fan_speed.value : 0;
    
    float speed_factor = 1.0;
    
    if (config.cooling) {
        #ifdef SLIC3R_DEBUG
        printf("Layer %zu estimated printing time: %f seconds\n", m_layer_id, elapsed);
        #endif        
        if (elapsed < (float)config.slowdown_below_layer_time) {
            // Layer time very short. Enable the fan to a full throttle and slow down the print
            // (stretch the layer print time to slowdown_below_layer_time).
            fan_speed = config.max_fan_speed;
            speed_factor = elapsed / (float)config.slowdown_below_layer_time;
        } else if (elapsed < (float)config.fan_below_layer_time) {
            // Layer time quite short. Enable the fan proportionally according to the current layer time.
            fan_speed = config.max_fan_speed
                - (config.max_fan_speed - config.min_fan_speed)
                * (elapsed - (float)config.slowdown_below_layer_time)
                / (config.fan_below_layer_time - config.slowdown_below_layer_time);
        }
        
        #ifdef SLIC3R_DEBUG
        printf("  fan = %d%%, speed = %f%%\n", fan_speed, speed_factor * 100);
        #endif
        
        if (speed_factor < 1.0) {
            // Adjust feed rate of G1 commands marked with an _EXTRUDE_SET_SPEED
            // as long as they are not _WIPE moves (they cannot if they are _EXTRUDE_SET_SPEED)
            // and they are not preceded directly by _BRIDGE_FAN_START (do not adjust bridging speed).
            std::string new_gcode;
            std::istringstream ss(gcode);
            std::string line;
            bool  bridge_fan_start = false;
            float min_print_speed  = float(config.min_print_speed * 60.);
            while (std::getline(ss, line)) {
                if (boost::starts_with(line, "G1")
                    && boost::contains(line, ";_EXTRUDE_SET_SPEED")
                    && !boost::contains(line, ";_WIPE")
                    && !bridge_fan_start) {
                    apply_speed_factor(line, speed_factor, min_print_speed);
                    boost::replace_first(line, ";_EXTRUDE_SET_SPEED", "");
                }
                bridge_fan_start = boost::contains(line, ";_BRIDGE_FAN_START");
                new_gcode += line + '\n';
            }
            gcode = new_gcode;
        }
    }
    if (m_layer_id < config.disable_fan_first_layers)
        fan_speed = 0;
    
    gcode = m_gcodegen.writer().set_fan(fan_speed) + gcode;
    
    // bridge fan speed
    if (!config.cooling || config.bridge_fan_speed == 0 || m_layer_id < config.disable_fan_first_layers) {
        boost::replace_all(gcode, ";_BRIDGE_FAN_START", "");
        boost::replace_all(gcode, ";_BRIDGE_FAN_END", "");
    } else {
        boost::replace_all(gcode, ";_BRIDGE_FAN_START", m_gcodegen.writer().set_fan(config.bridge_fan_speed, true));
        boost::replace_all(gcode, ";_BRIDGE_FAN_END",   m_gcodegen.writer().set_fan(fan_speed, true));
    }
    boost::replace_all(gcode, ";_WIPE", "");
    boost::replace_all(gcode, ";_EXTRUDE_SET_SPEED", "");
    
    m_object_ids_visited.clear();
    return gcode;
}

}
