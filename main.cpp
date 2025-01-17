#include <vector>
#include <iostream>
#include <optional>
#include <string_view>
#include <chrono>
using hr_clock = std::chrono::high_resolution_clock;

#include "opengl.h"

constexpr char vert_shader_source[] = R"(
uniform mat4 u_view_proj_matrix;
uniform mat4 u_model_matrix;
layout(location=0) in vec3 v_position;
layout(location=1) in vec3 v_normal;
layout(location=0) out vec3 position;
layout(location=1) out vec3 normal;
void main()
{
    position    = (u_model_matrix * vec4(v_position,1)).xyz;
    normal      = (u_model_matrix * vec4(v_normal,0)).xyz;
    gl_Position = u_view_proj_matrix * vec4(position,1);
})";

constexpr char frag_shader_source[] = R"(
// Fragment shader for untextured PBR surface
uniform vec3 u_albedo;
uniform float u_roughness;
uniform float u_metalness;
uniform float u_ambient_occlusion;
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=0) out vec4 f_color;
void main() 
{ 
    // Compute our PBR lighting
    vec3 light = compute_lighting(position, normal, u_albedo, u_roughness, u_metalness, u_ambient_occlusion);

    // Apply simple tone mapping and write to fragment
    f_color = vec4(light / (light + 1), 1);
})";

struct camera
{
    float3 position;
    float pitch=0, yaw=0;

    float4 get_orientation() const { return qmul(rotation_quat(float3{0,1,0}, yaw), rotation_quat(float3{1,0,0}, pitch)); }
    float4x4 get_view_matrix() const { return inverse(pose_matrix(get_orientation(), position)); }
    float4x4 get_skybox_view_matrix() const { return rotation_matrix(qconj(get_orientation())); }

    void move_local(const float3 & displacement) { position += qrot(get_orientation(), displacement); }
};

struct vertex { float3 position, normal; };
std::vector<vertex> make_sphere(int slices, int stacks, float radius)
{
    const auto make_vertex = [slices, stacks, radius](int i, int j)
    {
        const float tau = 6.2831853f, longitude = i*tau/slices, latitude = (j-(stacks*0.5f))*tau/2/stacks;
        const float3 normal {cos(longitude)*cos(latitude), sin(latitude), sin(longitude)*cos(latitude)}; // Poles at +/-y
        return vertex{normal*radius, normal};
    };

    std::vector<vertex> vertices;
    for(int i=0; i<slices; ++i)
    {
        for(int j=0; j<stacks; ++j)
        {
            vertices.push_back(make_vertex(i,j));
            vertices.push_back(make_vertex(i,j+1));
            vertices.push_back(make_vertex(i+1,j+1));
            vertices.push_back(make_vertex(i+1,j));
        }
    }
    return vertices;
}

#define STB_IMAGE_IMPLEMENTATION
#include "3rdparty/stb_image.h"

struct environment { GLuint environment_cubemap, irradiance_cubemap, reflectance_cubemap; };

environment load_enviroment(const pbr_tools & tools, const char * filename)
{
    // Load spheremap
    int width, height;
    float * pixels = stbi_loadf(filename, &width, &height, nullptr, 3);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(pixels);

    // Compute environment maps
    const GLuint environment_cubemap = tools.convert_spheremap_to_cubemap(GL_RGB16F, 1024, tex);
    const GLuint irradiance_cubemap = tools.compute_irradiance_map(environment_cubemap);
    const GLuint reflectance_cubemap = tools.compute_reflectance_map(environment_cubemap);
    glDeleteTextures(1, &tex);
    return {environment_cubemap, irradiance_cubemap, reflectance_cubemap};
}

int main() try
{
    auto sphere_verts = make_sphere(32,16,0.4f);
    constexpr float3 skybox_verts[]
    {
        {-1,-1,-1}, {-1,+1,-1}, {-1,+1,+1}, {-1,-1,+1},
        {+1,-1,-1}, {+1,-1,+1}, {+1,+1,+1}, {+1,+1,-1},
        {-1,-1,-1}, {-1,-1,+1}, {+1,-1,+1}, {+1,-1,-1},
        {-1,+1,-1}, {+1,+1,-1}, {+1,+1,+1}, {-1,+1,+1},
        {-1,-1,-1}, {+1,-1,-1}, {+1,+1,-1}, {-1,+1,-1},
        {-1,-1,+1}, {-1,+1,+1}, {+1,+1,+1}, {+1,-1,+1}
    };

    glfwInit();

    glfwWindowHint(GLFW_SAMPLES, 4);
    glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
    auto win = glfwCreateWindow(1280, 720, "PBR Test", nullptr, nullptr);
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    glewInit();
    pbr_tools tools;
    gl_program prog {compile_shader(GL_VERTEX_SHADER, {preamble, vert_shader_source}), 
                     compile_shader(GL_FRAGMENT_SHADER, {preamble, pbr_lighting, frag_shader_source})};

    // Set up a right-handed, x-right, y-down, z-forward coordinate system with a 0-to-1 depth buffer
    glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW); // Still actually counter-clockwise
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    int width, height;
    float * pixels = stbi_loadf("monument-valley.hdr", &width, &height, nullptr, 3);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(pixels);

    // Convert spheremap to cubemap
    const GLuint brdf_integration_map = tools.compute_brdf_integration_map();
    const environment env[3]
    {
        load_enviroment(tools, "monument-valley.hdr"),
        load_enviroment(tools, "factory-catwalk.hdr"),
        load_enviroment(tools, "shiodome-stairs.hdr"),
    };
    int env_index = 0;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glEnable(GL_FRAMEBUFFER_SRGB);
    glEnable(GL_DEPTH_TEST);

    // Initialize camera
    const float cam_speed = 8;
    camera cam {{0,0,-8}};
    double2 prev_cursor;
    glfwGetCursorPos(win, &prev_cursor.x, &prev_cursor.y);
    auto t0 = hr_clock::now();
    while(!glfwWindowShouldClose(win))
    {
        glfwPollEvents();

        // Handle user input
        double2 cursor;
        glfwGetCursorPos(win, &cursor.x, &cursor.y);
        if(glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT))
        {
            cam.yaw += static_cast<float>(cursor.x - prev_cursor.x) * 0.01f;
            cam.pitch += static_cast<float>(prev_cursor.y - cursor.y) * 0.01f;
        }
        prev_cursor = cursor;

        const auto t1 = hr_clock::now();
        const float timestep = std::chrono::duration<float>(t1-t0).count();
        t0 = t1;

        float3 move;
        if(glfwGetKey(win, GLFW_KEY_W)) move.z += 1;
        if(glfwGetKey(win, GLFW_KEY_A)) move.x -= 1;
        if(glfwGetKey(win, GLFW_KEY_S)) move.z -= 1;
        if(glfwGetKey(win, GLFW_KEY_D)) move.x += 1;
        if(length(move) > 0) cam.move_local(normalize(move) * (cam_speed * timestep));

        if(glfwGetKey(win, GLFW_KEY_1)) env_index=0;
        if(glfwGetKey(win, GLFW_KEY_2)) env_index=1;
        if(glfwGetKey(win, GLFW_KEY_3)) env_index=2;

        // Set up scene
        glfwGetFramebufferSize(win, &width, &height);
        glViewport(0, 0, width, height);
        glClear(GL_DEPTH_BUFFER_BIT);
        const float4x4 view_matrix = cam.get_view_matrix();
        const float4x4 proj_matrix = linalg::perspective_matrix(1.0f, (float)width/height, 0.1f, 32.0f, linalg::pos_z, linalg::zero_to_one);

        // Render skybox
        tools.draw_skybox(env[env_index].environment_cubemap, mul(proj_matrix, cam.get_skybox_view_matrix()));

        // Render spheres
        for(int i : {0,1}) glEnableVertexAttribArray(i);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), &sphere_verts.front().position[0]);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), &sphere_verts.front().normal[0]);

        prog.bind_texture("u_brdf_integration_map", brdf_integration_map);
        prog.bind_texture("u_irradiance_map", env[env_index].irradiance_cubemap);
        prog.bind_texture("u_reflectance_map", env[env_index].reflectance_cubemap);

        prog.use();
        prog.uniform("u_view_proj_matrix", mul(proj_matrix, view_matrix));
        prog.uniform("u_eye_position", cam.position);

        prog.uniform("u_ambient_occlusion", 1.0f);
        const float3 albedos[] {{1,1,1}, {1,0,0}, {1,1,0}, {0,1,0}, {0,1,1}, {0,0,1}, {1,0,1}};
        for(int i=0; i<7; ++i)
        {
            for(int j=0; j<7; ++j)
            {
                for(int k=0; k<7; ++k)
                {                    
                    prog.uniform("u_model_matrix", translation_matrix(float3{j-3.0f, i-3.0f, k-3.0f}));
                    prog.uniform("u_albedo", albedos[k]);
                    prog.uniform("u_metalness", 1-(i+0.5f)/7);
                    prog.uniform("u_roughness", (j+0.5f)/7);
                    glDrawArrays(GL_QUADS, 0, sphere_verts.size());
                }
            }
        }
        for(int i : {0,1}) glDisableVertexAttribArray(i);

        glfwSwapBuffers(win);
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    return EXIT_SUCCESS;
}
catch(const std::exception & e)
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}