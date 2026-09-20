#include "./shared.h"

// Neil Bartlett / Tanner Helland blackbody approximation (vanilla white balance)
float3 BlackbodyTint(float kelvin)
{
  float3 c;
  if (kelvin <= 6500.0)
  {
    c.r = 1.0;
    c.g = -2902.19556 / (kelvin + 1669.58032) + 1.33026743;
    c.b = -8257.79980 / (kelvin + 2575.28271) + 1.89937544;
  }
  else
  {
    c.r =  1745.04248 / (kelvin - 2666.34741) + 0.509953916;
    c.g =  1216.61682 / (kelvin - 2173.10132) + 0.703812003;
    c.b = -8257.79980 / (kelvin + 2575.28271) + 1.89937544;
  }
  c = saturate(c);

  // below 1000K the tint is faded back out to neutral
  float fade = saturate(-0.001 * (kelvin - 1000.0));
  fade = fade * fade * (3.0 - 2.0 * fade);
  return lerp(c, 1.0, fade);
}

float3 softclip(float3 color, float strength) {
    float3 clipped = saturate(color);
    clipped = clipped * clipped * (3.0 - 2.0 * clipped);
    return lerp(color, clipped, strength);
}

float3 softclipExtended(float3 color, float strength) {
    const float pivot = 0.18;

    // curve, evaluated only up to the pivot
    float3 c = min(color, pivot);
    float3 clipped = saturate(c);
    clipped = lerp(c, clipped * clipped * (3.0 - 2.0 * clipped), strength);

    // tangent slope at the pivot, analytic
    float slope = lerp(1.0, 6.0 * pivot * (1.0 - pivot), strength);

    return clipped + slope * max(color - pivot, 0.0);
}

// from Musa Haji
float3 anchoredCInfinityShoulder(float3 color, float3 peak, float3 anchor, float compressionStrength) {
  float3 shoulderRange = peak - anchor;
  float3 distanceFromAnchor = max(color - anchor, 0.f);
  float3 flatWeight = exp2(-renodx::math::DivideSafe(shoulderRange, compressionStrength * distanceFromAnchor));
  float3 responseDenominator = mad(distanceFromAnchor, flatWeight, shoulderRange);
  return mad(shoulderRange, renodx::math::DivideSafe(distanceFromAnchor, responseDenominator, 0.f.xxx), color - distanceFromAnchor);
}

static const float3x3 BT709_TO_XYZ_MAT = float3x3(
		0.4123907993f, 0.3575843394f, 0.1804807884f,
		0.2126390059f, 0.7151686788f, 0.0721923154f,
		0.0193308187f, 0.1191947798f, 0.9505321522f);

static const float3x3 XYZ_TO_BT709_MAT = float3x3(
		3.2409699419f, -1.5373831776f, -0.4986107603f,
		-0.9692436363f, 1.8759675015f, 0.0415550574f,
		0.0556300797f, -0.2039769589f, 1.0569715142f);

float3 XyYToXYZ(float3 xyY) {
	const float safe_y = max(xyY.y, 1e-5f);
	return float3(
			xyY.x / safe_y * xyY.z,
			xyY.z,
			(1.f - xyY.x - xyY.y) / safe_y * xyY.z);
}

float2 KelvinToKrystek1985UCSXY(float kelvin) {
	kelvin = clamp(kelvin, 1000.f, 15000.f);

	const float kelvin_squared = kelvin * kelvin;
	const float u = (0.860117757f + 1.54118254e-4f * kelvin + 1.28641212e-7f * kelvin_squared)
									/ (1.f + 8.42420235e-4f * kelvin + 7.08145163e-7f * kelvin_squared);
	const float v = (0.317398726f + 4.22806245e-5f * kelvin + 4.20481691e-8f * kelvin_squared)
									/ (1.f - 2.89741816e-5f * kelvin + 1.61456053e-7f * kelvin_squared);

	const float d = 1.f / (2.f * u - 8.f * v + 4.f);
	return float2(3.f * u * d, 2.f * v * d);
}

float2 KelvinToCIEJudd1964IlluminantDXY(float temperature) {
	temperature = clamp(temperature, 4000.f, 25000.f);

	const float x_d = temperature <= 7000.f
												? (-4.6070e9f / (temperature * temperature * temperature))
															+ (2.9678e6f / (temperature * temperature))
															+ (99.11f / temperature)
															+ 0.244063f
												: (-2.0064e9f / (temperature * temperature * temperature))
															+ (1.9018e6f / (temperature * temperature))
															+ (247.48f / temperature)
															+ 0.237040f;
	const float y_d = (-3.f * x_d * x_d) + (2.87f * x_d) - 0.275f;
	return float2(x_d, y_d);
}

float3 KelvinToWhiteXYZ(float kelvin, bool daylight_method) {
	const float2 xy = daylight_method
												? KelvinToCIEJudd1964IlluminantDXY(kelvin)
												: KelvinToKrystek1985UCSXY(kelvin);
	return XyYToXYZ(float3(xy, 1.f));
}

float3x3 ChromaticAdaptationMatrix(float3 source_white_xyz, float3 destination_white_xyz) {
	const float3x3 bradford = float3x3(
			0.8951f, 0.2664f, -0.1614f,
			-0.7502f, 1.7135f, 0.0367f,
			0.0389f, -0.0685f, 1.0296f);

	const float3x3 bradford_inverse = float3x3(
			0.9869929f, -0.1470543f, 0.1599627f,
			0.4323053f, 0.5183603f, 0.0492912f,
			-0.0085287f, 0.0400428f, 0.9684867f);

	const float3 source_lms = mul(bradford, source_white_xyz);
	const float3 destination_lms = mul(bradford, destination_white_xyz);
	const float3 scale = destination_lms / max(source_lms, 1e-5f);

	const float3x3 scale_matrix = float3x3(
			scale.x, 0.f, 0.f,
			0.f, scale.y, 0.f,
			0.f, 0.f, scale.z);

	return mul(bradford_inverse, mul(scale_matrix, bradford));
}

float3 AdaptXYZKelvin(float3 xyz_color, float source_kelvin, float destination_kelvin, bool daylight_method) {
	const float3 source_white_xyz = KelvinToWhiteXYZ(source_kelvin, daylight_method);
	const float3 destination_white_xyz = KelvinToWhiteXYZ(destination_kelvin, daylight_method);
	return mul(ChromaticAdaptationMatrix(source_white_xyz, destination_white_xyz), xyz_color);
}

float3 AdaptBT709Kelvin(float3 bt709_color, float source_kelvin, float destination_kelvin, bool daylight_method) {
	float3 xyz_color = mul(BT709_TO_XYZ_MAT, bt709_color);
	xyz_color = AdaptXYZKelvin(xyz_color, source_kelvin, destination_kelvin, daylight_method);
	return mul(XYZ_TO_BT709_MAT, xyz_color);
}

float3 AdaptBT709Kelvin(float3 bt709_color, float destination_kelvin) {
	return AdaptBT709Kelvin(bt709_color, 6500.f, destination_kelvin, false);
}