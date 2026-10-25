// The same framework-owned source is compiled twice by the pinned DXC contract.
// No generated SPIR-V is checked in.

#if defined(VITARECOMP_VERTEX_SHADER)

struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

VertexOutput main(uint vertex_id : SV_VertexID) {
    static const float2 positions[3] = {
        float2(-0.75, 0.70),
        float2(0.75, 0.70),
        float2(0.0, -0.75),
    };
    static const float3 colors[3] = {
        float3(1.0, 0.1, 0.1),
        float3(0.1, 1.0, 0.1),
        float3(0.1, 0.1, 1.0),
    };
    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.color = colors[vertex_id];
    return output;
}

#else

float4 main(float3 color : COLOR0) : SV_Target0 {
    return float4(color, 1.0);
}

#endif
