// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolPipelineRecord.h"

#include "json.hpp"

#include <vector>

using namespace jasperengine;
using json = nlohmann::json;

namespace {

    template<typename E>
    struct EnumName {
        E value;
        const char* token;
    };

    // sokol's enum names, lower case without the prefix ("rgba8", "less_equal"); the sokol default
    // of each enum is "default". By name, never by value: a sokol update may renumber them (S14
    // inserted the ASTC sizes)
    template<typename E>
    const std::vector<EnumName<E>>& names();

    template<> const std::vector<EnumName<sg_pixel_format>>& names<sg_pixel_format>(){
        static const std::vector<EnumName<sg_pixel_format>> table = {
            {_SG_PIXELFORMAT_DEFAULT, "default"},
            {SG_PIXELFORMAT_NONE, "none"},
            {SG_PIXELFORMAT_R8, "r8"},
            {SG_PIXELFORMAT_R8SN, "r8sn"},
            {SG_PIXELFORMAT_R8UI, "r8ui"},
            {SG_PIXELFORMAT_R8SI, "r8si"},
            {SG_PIXELFORMAT_R16, "r16"},
            {SG_PIXELFORMAT_R16SN, "r16sn"},
            {SG_PIXELFORMAT_R16UI, "r16ui"},
            {SG_PIXELFORMAT_R16SI, "r16si"},
            {SG_PIXELFORMAT_R16F, "r16f"},
            {SG_PIXELFORMAT_RG8, "rg8"},
            {SG_PIXELFORMAT_RG8SN, "rg8sn"},
            {SG_PIXELFORMAT_RG8UI, "rg8ui"},
            {SG_PIXELFORMAT_RG8SI, "rg8si"},
            {SG_PIXELFORMAT_R32UI, "r32ui"},
            {SG_PIXELFORMAT_R32SI, "r32si"},
            {SG_PIXELFORMAT_R32F, "r32f"},
            {SG_PIXELFORMAT_RG16, "rg16"},
            {SG_PIXELFORMAT_RG16SN, "rg16sn"},
            {SG_PIXELFORMAT_RG16UI, "rg16ui"},
            {SG_PIXELFORMAT_RG16SI, "rg16si"},
            {SG_PIXELFORMAT_RG16F, "rg16f"},
            {SG_PIXELFORMAT_RGBA8, "rgba8"},
            {SG_PIXELFORMAT_SRGB8A8, "srgb8a8"},
            {SG_PIXELFORMAT_RGBA8SN, "rgba8sn"},
            {SG_PIXELFORMAT_RGBA8UI, "rgba8ui"},
            {SG_PIXELFORMAT_RGBA8SI, "rgba8si"},
            {SG_PIXELFORMAT_BGRA8, "bgra8"},
            {SG_PIXELFORMAT_RGB10A2, "rgb10a2"},
            {SG_PIXELFORMAT_RG11B10F, "rg11b10f"},
            {SG_PIXELFORMAT_RGB9E5, "rgb9e5"},
            {SG_PIXELFORMAT_RG32UI, "rg32ui"},
            {SG_PIXELFORMAT_RG32SI, "rg32si"},
            {SG_PIXELFORMAT_RG32F, "rg32f"},
            {SG_PIXELFORMAT_RGBA16, "rgba16"},
            {SG_PIXELFORMAT_RGBA16SN, "rgba16sn"},
            {SG_PIXELFORMAT_RGBA16UI, "rgba16ui"},
            {SG_PIXELFORMAT_RGBA16SI, "rgba16si"},
            {SG_PIXELFORMAT_RGBA16F, "rgba16f"},
            {SG_PIXELFORMAT_RGBA32UI, "rgba32ui"},
            {SG_PIXELFORMAT_RGBA32SI, "rgba32si"},
            {SG_PIXELFORMAT_RGBA32F, "rgba32f"},
            {SG_PIXELFORMAT_RGB565, "rgb565"},
            {SG_PIXELFORMAT_RGBA4, "rgba4"},
            {SG_PIXELFORMAT_DEPTH, "depth"},
            {SG_PIXELFORMAT_DEPTH_STENCIL, "depth_stencil"},
            {SG_PIXELFORMAT_BC1_RGBA, "bc1_rgba"},
            {SG_PIXELFORMAT_BC2_RGBA, "bc2_rgba"},
            {SG_PIXELFORMAT_BC3_RGBA, "bc3_rgba"},
            {SG_PIXELFORMAT_BC3_SRGBA, "bc3_srgba"},
            {SG_PIXELFORMAT_BC4_R, "bc4_r"},
            {SG_PIXELFORMAT_BC4_RSN, "bc4_rsn"},
            {SG_PIXELFORMAT_BC5_RG, "bc5_rg"},
            {SG_PIXELFORMAT_BC5_RGSN, "bc5_rgsn"},
            {SG_PIXELFORMAT_BC6H_RGBF, "bc6h_rgbf"},
            {SG_PIXELFORMAT_BC6H_RGBUF, "bc6h_rgbuf"},
            {SG_PIXELFORMAT_BC7_RGBA, "bc7_rgba"},
            {SG_PIXELFORMAT_BC7_SRGBA, "bc7_srgba"},
            {SG_PIXELFORMAT_ETC2_RGB8, "etc2_rgb8"},
            {SG_PIXELFORMAT_ETC2_SRGB8, "etc2_srgb8"},
            {SG_PIXELFORMAT_ETC2_RGB8A1, "etc2_rgb8a1"},
            {SG_PIXELFORMAT_ETC2_RGBA8, "etc2_rgba8"},
            {SG_PIXELFORMAT_ETC2_SRGB8A8, "etc2_srgb8a8"},
            {SG_PIXELFORMAT_EAC_R11, "eac_r11"},
            {SG_PIXELFORMAT_EAC_R11SN, "eac_r11sn"},
            {SG_PIXELFORMAT_EAC_RG11, "eac_rg11"},
            {SG_PIXELFORMAT_EAC_RG11SN, "eac_rg11sn"},
            {SG_PIXELFORMAT_ASTC_4x4_RGBA, "astc_4x4_rgba"},
            {SG_PIXELFORMAT_ASTC_4x4_SRGBA, "astc_4x4_srgba"},
            {SG_PIXELFORMAT_ASTC_5x5_RGBA, "astc_5x5_rgba"},
            {SG_PIXELFORMAT_ASTC_5x5_SRGBA, "astc_5x5_srgba"},
            {SG_PIXELFORMAT_ASTC_6x6_RGBA, "astc_6x6_rgba"},
            {SG_PIXELFORMAT_ASTC_6x6_SRGBA, "astc_6x6_srgba"},
            {SG_PIXELFORMAT_ASTC_8x8_RGBA, "astc_8x8_rgba"},
            {SG_PIXELFORMAT_ASTC_8x8_SRGBA, "astc_8x8_srgba"},
            {SG_PIXELFORMAT_ASTC_10x10_RGBA, "astc_10x10_rgba"},
            {SG_PIXELFORMAT_ASTC_10x10_SRGBA, "astc_10x10_srgba"},
            {SG_PIXELFORMAT_ASTC_12x12_RGBA, "astc_12x12_rgba"},
            {SG_PIXELFORMAT_ASTC_12x12_SRGBA, "astc_12x12_srgba"},
        };
        return table;
    }

    template<> const std::vector<EnumName<sg_vertex_format>>& names<sg_vertex_format>(){
        static const std::vector<EnumName<sg_vertex_format>> table = {
            {SG_VERTEXFORMAT_INVALID, "invalid"},
            {SG_VERTEXFORMAT_FLOAT, "float"},
            {SG_VERTEXFORMAT_FLOAT2, "float2"},
            {SG_VERTEXFORMAT_FLOAT3, "float3"},
            {SG_VERTEXFORMAT_FLOAT4, "float4"},
            {SG_VERTEXFORMAT_INT, "int"},
            {SG_VERTEXFORMAT_INT2, "int2"},
            {SG_VERTEXFORMAT_INT3, "int3"},
            {SG_VERTEXFORMAT_INT4, "int4"},
            {SG_VERTEXFORMAT_UINT, "uint"},
            {SG_VERTEXFORMAT_UINT2, "uint2"},
            {SG_VERTEXFORMAT_UINT3, "uint3"},
            {SG_VERTEXFORMAT_UINT4, "uint4"},
            {SG_VERTEXFORMAT_BYTE4, "byte4"},
            {SG_VERTEXFORMAT_BYTE4N, "byte4n"},
            {SG_VERTEXFORMAT_UBYTE4, "ubyte4"},
            {SG_VERTEXFORMAT_UBYTE4N, "ubyte4n"},
            {SG_VERTEXFORMAT_SHORT2, "short2"},
            {SG_VERTEXFORMAT_SHORT2N, "short2n"},
            {SG_VERTEXFORMAT_USHORT2, "ushort2"},
            {SG_VERTEXFORMAT_USHORT2N, "ushort2n"},
            {SG_VERTEXFORMAT_SHORT4, "short4"},
            {SG_VERTEXFORMAT_SHORT4N, "short4n"},
            {SG_VERTEXFORMAT_USHORT4, "ushort4"},
            {SG_VERTEXFORMAT_USHORT4N, "ushort4n"},
            {SG_VERTEXFORMAT_INT10_N2, "int10_n2"},
            {SG_VERTEXFORMAT_UINT10_N2, "uint10_n2"},
            {SG_VERTEXFORMAT_HALF2, "half2"},
            {SG_VERTEXFORMAT_HALF4, "half4"},
        };
        return table;
    }

    template<> const std::vector<EnumName<sg_blend_factor>>& names<sg_blend_factor>(){
        static const std::vector<EnumName<sg_blend_factor>> table = {
            {_SG_BLENDFACTOR_DEFAULT, "default"},
            {SG_BLENDFACTOR_ZERO, "zero"},
            {SG_BLENDFACTOR_ONE, "one"},
            {SG_BLENDFACTOR_SRC_COLOR, "src_color"},
            {SG_BLENDFACTOR_ONE_MINUS_SRC_COLOR, "one_minus_src_color"},
            {SG_BLENDFACTOR_SRC_ALPHA, "src_alpha"},
            {SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, "one_minus_src_alpha"},
            {SG_BLENDFACTOR_DST_COLOR, "dst_color"},
            {SG_BLENDFACTOR_ONE_MINUS_DST_COLOR, "one_minus_dst_color"},
            {SG_BLENDFACTOR_DST_ALPHA, "dst_alpha"},
            {SG_BLENDFACTOR_ONE_MINUS_DST_ALPHA, "one_minus_dst_alpha"},
            {SG_BLENDFACTOR_SRC_ALPHA_SATURATED, "src_alpha_saturated"},
            {SG_BLENDFACTOR_BLEND_COLOR, "blend_color"},
            {SG_BLENDFACTOR_ONE_MINUS_BLEND_COLOR, "one_minus_blend_color"},
            {SG_BLENDFACTOR_BLEND_ALPHA, "blend_alpha"},
            {SG_BLENDFACTOR_ONE_MINUS_BLEND_ALPHA, "one_minus_blend_alpha"},
            {SG_BLENDFACTOR_SRC1_COLOR, "src1_color"},
            {SG_BLENDFACTOR_ONE_MINUS_SRC1_COLOR, "one_minus_src1_color"},
            {SG_BLENDFACTOR_SRC1_ALPHA, "src1_alpha"},
            {SG_BLENDFACTOR_ONE_MINUS_SRC1_ALPHA, "one_minus_src1_alpha"},
        };
        return table;
    }

    template<> const std::vector<EnumName<sg_blend_op>>& names<sg_blend_op>(){
        static const std::vector<EnumName<sg_blend_op>> table = {
            {_SG_BLENDOP_DEFAULT, "default"},
            {SG_BLENDOP_ADD, "add"},
            {SG_BLENDOP_SUBTRACT, "subtract"},
            {SG_BLENDOP_REVERSE_SUBTRACT, "reverse_subtract"},
            {SG_BLENDOP_MIN, "min"},
            {SG_BLENDOP_MAX, "max"},
        };
        return table;
    }

    template<> const std::vector<EnumName<sg_stencil_op>>& names<sg_stencil_op>(){
        static const std::vector<EnumName<sg_stencil_op>> table = {
            {_SG_STENCILOP_DEFAULT, "default"},
            {SG_STENCILOP_KEEP, "keep"},
            {SG_STENCILOP_ZERO, "zero"},
            {SG_STENCILOP_REPLACE, "replace"},
            {SG_STENCILOP_INCR_CLAMP, "incr_clamp"},
            {SG_STENCILOP_DECR_CLAMP, "decr_clamp"},
            {SG_STENCILOP_INVERT, "invert"},
            {SG_STENCILOP_INCR_WRAP, "incr_wrap"},
            {SG_STENCILOP_DECR_WRAP, "decr_wrap"},
        };
        return table;
    }

    template<> const std::vector<EnumName<sg_compare_func>>& names<sg_compare_func>(){
        static const std::vector<EnumName<sg_compare_func>> table = {
            {_SG_COMPAREFUNC_DEFAULT, "default"},
            {SG_COMPAREFUNC_NEVER, "never"},
            {SG_COMPAREFUNC_LESS, "less"},
            {SG_COMPAREFUNC_EQUAL, "equal"},
            {SG_COMPAREFUNC_LESS_EQUAL, "less_equal"},
            {SG_COMPAREFUNC_GREATER, "greater"},
            {SG_COMPAREFUNC_NOT_EQUAL, "not_equal"},
            {SG_COMPAREFUNC_GREATER_EQUAL, "greater_equal"},
            {SG_COMPAREFUNC_ALWAYS, "always"},
        };
        return table;
    }

    template<> const std::vector<EnumName<sg_primitive_type>>& names<sg_primitive_type>(){
        static const std::vector<EnumName<sg_primitive_type>> table = {
            {_SG_PRIMITIVETYPE_DEFAULT, "default"},
            {SG_PRIMITIVETYPE_POINTS, "points"},
            {SG_PRIMITIVETYPE_LINES, "lines"},
            {SG_PRIMITIVETYPE_LINE_STRIP, "line_strip"},
            {SG_PRIMITIVETYPE_TRIANGLES, "triangles"},
            {SG_PRIMITIVETYPE_TRIANGLE_STRIP, "triangle_strip"},
        };
        return table;
    }

    template<> const std::vector<EnumName<sg_index_type>>& names<sg_index_type>(){
        static const std::vector<EnumName<sg_index_type>> table = {
            {_SG_INDEXTYPE_DEFAULT, "default"},
            {SG_INDEXTYPE_NONE, "none"},
            {SG_INDEXTYPE_UINT16, "uint16"},
            {SG_INDEXTYPE_UINT32, "uint32"},
        };
        return table;
    }

    template<> const std::vector<EnumName<sg_cull_mode>>& names<sg_cull_mode>(){
        static const std::vector<EnumName<sg_cull_mode>> table = {
            {_SG_CULLMODE_DEFAULT, "default"},
            {SG_CULLMODE_NONE, "none"},
            {SG_CULLMODE_FRONT, "front"},
            {SG_CULLMODE_BACK, "back"},
        };
        return table;
    }

    template<> const std::vector<EnumName<sg_face_winding>>& names<sg_face_winding>(){
        static const std::vector<EnumName<sg_face_winding>> table = {
            {_SG_FACEWINDING_DEFAULT, "default"},
            {SG_FACEWINDING_CCW, "ccw"},
            {SG_FACEWINDING_CW, "cw"},
        };
        return table;
    }

    template<> const std::vector<EnumName<sg_vertex_step>>& names<sg_vertex_step>(){
        static const std::vector<EnumName<sg_vertex_step>> table = {
            {_SG_VERTEXSTEP_DEFAULT, "default"},
            {SG_VERTEXSTEP_PER_VERTEX, "per_vertex"},
            {SG_VERTEXSTEP_PER_INSTANCE, "per_instance"},
        };
        return table;
    }

    template<typename E>
    const char* tokenOf(E value){
        for (const EnumName<E>& entry : names<E>()){
            if (entry.value == value)
                return entry.token;
        }
        return "default";
    }

    template<typename E>
    bool valueOf(const json& node, const char* key, E& out, std::string* error){
        if (!node.contains(key)){
            out = (E)0;
            return true;
        }
        if (!node[key].is_string()){
            if (error) *error = std::string(key) + " is not a token";
            return false;
        }
        const std::string token = node[key].get<std::string>();
        for (const EnumName<E>& entry : names<E>()){
            if (token == entry.token){
                out = entry.value;
                return true;
            }
        }
        if (error) *error = "unknown " + std::string(key) + " '" + token + "'";
        return false;
    }

    template<typename T>
    bool numberOf(const json& node, const char* key, T& out, std::string* error){
        if (!node.contains(key)){
            out = T();
            return true;
        }
        if (!node[key].is_number() && !node[key].is_boolean()){
            if (error) *error = std::string(key) + " is not a number";
            return false;
        }
        out = node[key].is_boolean() ? (T)node[key].get<bool>() : node[key].get<T>();
        return true;
    }

    json blendJson(const sg_blend_state& b){
        json out;
        out["enabled"] = b.enabled;
        out["src_rgb"] = tokenOf(b.src_factor_rgb);
        out["dst_rgb"] = tokenOf(b.dst_factor_rgb);
        out["op_rgb"] = tokenOf(b.op_rgb);
        out["src_alpha"] = tokenOf(b.src_factor_alpha);
        out["dst_alpha"] = tokenOf(b.dst_factor_alpha);
        out["op_alpha"] = tokenOf(b.op_alpha);
        return out;
    }

    json faceJson(const sg_stencil_face_state& f){
        json out;
        out["compare"] = tokenOf(f.compare);
        out["fail"] = tokenOf(f.fail_op);
        out["depth_fail"] = tokenOf(f.depth_fail_op);
        out["pass"] = tokenOf(f.pass_op);
        return out;
    }

    bool faceOf(const json& node, sg_stencil_face_state& f, std::string* error){
        return valueOf(node, "compare", f.compare, error) && valueOf(node, "fail", f.fail_op, error) &&
               valueOf(node, "depth_fail", f.depth_fail_op, error) && valueOf(node, "pass", f.pass_op, error);
    }

}

std::string SokolPipelineRecord::toJson(const sg_pipeline_desc& d){
    json out;
    out["primitive"] = tokenOf(d.primitive_type);
    out["index"] = tokenOf(d.index_type);
    out["cull"] = tokenOf(d.cull_mode);
    out["winding"] = tokenOf(d.face_winding);
    out["samples"] = d.sample_count;
    out["alpha_to_coverage"] = d.alpha_to_coverage_enabled;
    out["blend_color"] = json::array({d.blend_color.r, d.blend_color.g, d.blend_color.b, d.blend_color.a});
    json buffers = json::array();
    for (const sg_vertex_buffer_layout_state& b : d.layout.buffers){
        if (b.stride == 0 && b.step_func == _SG_VERTEXSTEP_DEFAULT && b.step_rate == 0)
            break;
        buffers.push_back(json{{"stride", b.stride}, {"step", tokenOf(b.step_func)}, {"step_rate", b.step_rate}});
    }
    json attrs = json::array();
    for (const sg_vertex_attr_state& a : d.layout.attrs){
        if (a.format == SG_VERTEXFORMAT_INVALID)
            break;
        attrs.push_back(json{{"buffer", a.buffer_index}, {"offset", a.offset}, {"format", tokenOf(a.format)}});
    }
    out["layout"] = json{{"buffers", buffers}, {"attrs", attrs}};
    out["depth"] = json{{"format", tokenOf(d.depth.pixel_format)}, {"compare", tokenOf(d.depth.compare)},
                        {"write", d.depth.write_enabled},
                        {"bias", json::array({d.depth.bias, d.depth.bias_slope_scale, d.depth.bias_clamp})}};
    // written whether it is enabled or not: interning compares every field
    out["stencil"] = json{{"enabled", d.stencil.enabled}, {"front", faceJson(d.stencil.front)}, {"back", faceJson(d.stencil.back)},
                          {"read_mask", d.stencil.read_mask}, {"write_mask", d.stencil.write_mask}, {"ref", d.stencil.ref}};
    out["color_count"] = d.color_count;
    json colors = json::array();
    for (int i = 0; i < SG_MAX_COLOR_ATTACHMENTS; i++){
        const sg_color_target_state& c = d.colors[i];
        if (i >= d.color_count && c.pixel_format == _SG_PIXELFORMAT_DEFAULT && c.write_mask == _SG_COLORMASK_DEFAULT && !c.blend.enabled)
            continue;
        colors.push_back(json{{"slot", i}, {"format", tokenOf(c.pixel_format)}, {"mask", (int)c.write_mask}, {"blend", blendJson(c.blend)}});
    }
    out["colors"] = colors;
    return out.dump();
}

namespace {

    bool parseRecord(const std::string& text, sg_pipeline_desc& d, std::string* error){
        d = {};
        const json node = json::parse(text, nullptr, false);
        if (node.is_discarded() || !node.is_object()){
            if (error) *error = "not a JSON object";
            return false;
        }
        if (!valueOf(node, "primitive", d.primitive_type, error) || !valueOf(node, "index", d.index_type, error) ||
                !valueOf(node, "cull", d.cull_mode, error) || !valueOf(node, "winding", d.face_winding, error) ||
                !numberOf(node, "samples", d.sample_count, error) || !numberOf(node, "alpha_to_coverage", d.alpha_to_coverage_enabled, error) ||
                !numberOf(node, "color_count", d.color_count, error))
            return false;
        if (node.contains("blend_color") && node["blend_color"].is_array() && node["blend_color"].size() == 4){
            d.blend_color = {node["blend_color"][0].get<float>(), node["blend_color"][1].get<float>(),
                             node["blend_color"][2].get<float>(), node["blend_color"][3].get<float>()};
        }
        if (d.color_count < 0 || d.color_count > SG_MAX_COLOR_ATTACHMENTS){
            if (error) *error = "color_count out of range";
            return false;
        }
        const json layout = node.value("layout", json::object());
        const json buffers = layout.value("buffers", json::array());
        const json attrs = layout.value("attrs", json::array());
        if (!buffers.is_array() || buffers.size() > SG_MAX_VERTEXBUFFER_BINDSLOTS || !attrs.is_array() || attrs.size() > SG_MAX_VERTEX_ATTRIBUTES){
            if (error) *error = "layout out of range";
            return false;
        }
        for (size_t i = 0; i < buffers.size(); i++){
            sg_vertex_buffer_layout_state& b = d.layout.buffers[i];
            if (!numberOf(buffers[i], "stride", b.stride, error) || !valueOf(buffers[i], "step", b.step_func, error) ||
                    !numberOf(buffers[i], "step_rate", b.step_rate, error))
                return false;
        }
        for (size_t i = 0; i < attrs.size(); i++){
            sg_vertex_attr_state& a = d.layout.attrs[i];
            if (!numberOf(attrs[i], "buffer", a.buffer_index, error) || !numberOf(attrs[i], "offset", a.offset, error) ||
                    !valueOf(attrs[i], "format", a.format, error))
                return false;
            if (a.buffer_index < 0 || a.buffer_index >= SG_MAX_VERTEXBUFFER_BINDSLOTS){
                if (error) *error = "attribute buffer out of range";
                return false;
            }
        }
        const json depth = node.value("depth", json::object());
        if (!valueOf(depth, "format", d.depth.pixel_format, error) || !valueOf(depth, "compare", d.depth.compare, error) ||
                !numberOf(depth, "write", d.depth.write_enabled, error))
            return false;
        if (depth.contains("bias") && depth["bias"].is_array() && depth["bias"].size() == 3){
            d.depth.bias = depth["bias"][0].get<float>();
            d.depth.bias_slope_scale = depth["bias"][1].get<float>();
            d.depth.bias_clamp = depth["bias"][2].get<float>();
        }
        if (node.contains("stencil") && node["stencil"].is_object()){
            const json& stencil = node["stencil"];
            // a stencil object without "enabled" is an enabled one
            d.stencil.enabled = stencil.value("enabled", true);
            if (!faceOf(stencil.value("front", json::object()), d.stencil.front, error) ||
                    !faceOf(stencil.value("back", json::object()), d.stencil.back, error) ||
                    !numberOf(stencil, "read_mask", d.stencil.read_mask, error) || !numberOf(stencil, "write_mask", d.stencil.write_mask, error) ||
                    !numberOf(stencil, "ref", d.stencil.ref, error))
                return false;
        }
        const json colors = node.value("colors", json::array());
        if (!colors.is_array()){
            if (error) *error = "colors is not an array";
            return false;
        }
        for (const json& c : colors){
            int slot = 0;
            if (!numberOf(c, "slot", slot, error))
                return false;
            if (slot < 0 || slot >= SG_MAX_COLOR_ATTACHMENTS){
                if (error) *error = "color slot out of range";
                return false;
            }
            sg_color_target_state& target = d.colors[slot];
            int mask = 0;
            const json blend = c.value("blend", json::object());
            if (!valueOf(c, "format", target.pixel_format, error) || !numberOf(c, "mask", mask, error) ||
                    !numberOf(blend, "enabled", target.blend.enabled, error) ||
                    !valueOf(blend, "src_rgb", target.blend.src_factor_rgb, error) || !valueOf(blend, "dst_rgb", target.blend.dst_factor_rgb, error) ||
                    !valueOf(blend, "op_rgb", target.blend.op_rgb, error) || !valueOf(blend, "src_alpha", target.blend.src_factor_alpha, error) ||
                    !valueOf(blend, "dst_alpha", target.blend.dst_factor_alpha, error) || !valueOf(blend, "op_alpha", target.blend.op_alpha, error))
                return false;
            target.write_mask = (sg_color_mask)mask;
        }
        return true;
    }

}

bool SokolPipelineRecord::fromJson(const std::string& text, sg_pipeline_desc& desc, std::string* error){
    // a value of the wrong JSON type throws in nlohmann's accessors: a malformed entry, skipped
    try{
        return parseRecord(text, desc, error);
    }catch (const json::exception& e){
        if (error) *error = e.what();
        desc = {};
        return false;
    }
}
