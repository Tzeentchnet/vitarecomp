struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

VertexOutput vs_main(uint vertex_id : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-0.70, -0.65),
        float2( 0.00,  0.70),
        float2( 0.70, -0.65)
    };
    static const float3 colors[3] = {
        float3(1.0, 0.1, 0.1),
        float3(0.1, 1.0, 0.1),
        float3(0.1, 0.1, 1.0)
    };

    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.color = colors[vertex_id];
    return output;
}

float4 ps_main(VertexOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
