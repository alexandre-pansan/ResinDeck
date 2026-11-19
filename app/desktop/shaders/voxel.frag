#version 440

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vPosition;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Frame {
    mat4 mvp;
    vec4 modelColor;
    vec4 lineColor;
    vec4 lightDir;
    vec4 eye;
    float clipZ;
};

void main()
{
    if (dot(vNormal, vNormal) < 0.25) {  // the plate's lines: never cut
        fragColor = lineColor;
        return;
    }
    if (vPosition.z > clipZ + 0.0001)
        discard;

    vec3 n = normalize(vNormal);
    vec3 v = normalize(eye.xyz - vPosition);
    // the inside, seen where the model is cut at a layer: light it as a surface facing the camera
    float inside = 0.0;
    if (dot(n, v) < 0.0) {
        n = -n;
        inside = 1.0;
    }
    vec3 key = normalize(lightDir.xyz);
    vec3 fill = normalize(vec3(-0.6, 0.5, 0.35));
    float diffuse = 0.75 * max(dot(n, key), 0.0) + 0.3 * max(dot(n, fill), 0.0);
    float sky = 0.5 + 0.5 * n.z;  // brighter from above, like a room
    float specular = pow(max(dot(n, normalize(key + v)), 0.0), 40.0) * 0.25;
    vec3 color = modelColor.rgb * (0.18 + 0.14 * sky + 0.72 * diffuse) + vec3(specular);
    fragColor = vec4(mix(color, color * 0.55, inside), 1.0);
}
