#version 440

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;  // zero for the plate's lines

layout(location = 0) out vec3 vNormal;
layout(location = 1) out float vZ;

layout(std140, binding = 0) uniform Frame {
    mat4 mvp;
    vec4 modelColor;
    vec4 lineColor;
    vec4 lightDir;
    float clipZ;  // mm: the model is cut above the chosen layer
};

void main()
{
    vNormal = normal;
    vZ = position.z;
    gl_Position = mvp * vec4(position, 1.0);
}
