#version 440

layout(location = 0) in vec3 vNormal;
layout(location = 1) in float vZ;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Frame {
    mat4 mvp;
    vec4 modelColor;
    vec4 lineColor;
    vec4 lightDir;
    float clipZ;
};

void main()
{
    if (dot(vNormal, vNormal) < 0.5) {  // the plate's lines: never cut
        fragColor = lineColor;
        return;
    }
    if (vZ > clipZ + 0.0001)
        discard;
    float diffuse = max(dot(normalize(vNormal), normalize(lightDir.xyz)), 0.0);
    fragColor = vec4(modelColor.rgb * (0.35 + 0.65 * diffuse), 1.0);
}
