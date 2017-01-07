
cbuffer camera: register(b0)
{
	float4x4 camera_proj;
};

struct VS_IN
{
	float3 pos: POSITION;
	float2 tex: TEXCOORD;
};

struct PS_IN
{
	float4 pos: SV_POSITION;
	float2 tex: TEXCOORD;
};

PS_IN main(VS_IN input)
{
	PS_IN output = (PS_IN)0;

	output.pos = mul(camera_proj, float4(input.pos.xyz, 1));
	output.tex = input.tex;

	return output;
}
