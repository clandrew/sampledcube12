Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> g_feedbackMap : register(u0);

// Per-pixel color data passed through the pixel shader.
struct PixelShaderInput
{
	float4 pos : SV_POSITION;
	float3 color : COLOR0;
	float2 uv : TEXCOORD;
};

// A pass-through function for the (interpolated) color data.
float4 main(PixelShaderInput input) : SV_TARGET
{
	g_feedbackMap.WriteSamplerFeedback(g_texture, g_sampler, input.uv);
	//g_feedbackMap.WriteSamplerFeedbackLevel(g_texture, g_sampler, input.uv, 0);

	return g_texture.Sample(g_sampler, input.uv);
}
