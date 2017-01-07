
Texture2D image_texture;
SamplerState image_sampler;

struct PS_IN
{
	float4 pos: SV_POSITION;
	float2 tex: TEXCOORD0;
};

float4 main(PS_IN input) : SV_TARGET
{
	return image_texture.Sample(image_sampler, input.tex.xy);
}
