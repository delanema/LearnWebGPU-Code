/**
 * This file is part of the "Learn WebGPU for C++" book.
 *   https://github.com/eliemichel/LearnWebGPU
 *
 * MIT License
 * Copyright (c) 2022-2023 Elie Michel
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <webgpu/webgpu.hpp>

#include <glm/glm.hpp>

#include <filesystem>

class ResourceManager {
public:
	using vec3 = glm::vec3;
	using vec2 = glm::vec2;

	/**
	 * A structure that describes the data layout in the vertex buffer,
	 * used by loadGeometryFromObj and used it in `sizeof` and `offsetof`
	 * when uploading data to the GPU.
	 */
	struct VertexAttributes {
		vec3 position;

		// Texture mapping attributes represent the local frame in which
		// normals sampled from the normal map are expressed.
		vec3 tangent; // X axis
		vec3 bitangent; // Y axis
		vec3 normal; // Z axis

		vec3 color;
		vec2 uv;
	};

	using path = std::filesystem::path;

	// Load a shader from a WGSL file into a new shader module
	static wgpu::ShaderModule loadShaderModule(const path& path, wgpu::Device device);

	// Load an 3D mesh from a standard .obj file into a vertex data buffer (to be used with PrimitiveTopology::TriangleList)
	static bool loadGeometryFromObj(const path& path, std::vector<VertexAttributes>& vertexData);

	// Same as loadGeometryFromObj but creates a buffer to be used with PrimitiveTopology::LineList
	// NB: There is no mechanism to deduplicate edges from neighbor triangles
	static bool loadWireframeGeometryFromObj(const path& path, std::vector<VertexAttributes>& vertexData);

	// Load an image from a standard 8-bit image file into a new texture object
	// NB: The texture must be destroyed after use
	static wgpu::Texture loadTexture(const path& path, wgpu::Device device, wgpu::TextureView* pTextureView = nullptr);

	// Load a cubemap and all its MIP levels that correspond to various roughnesses
	static wgpu::Texture loadPrefilteredCubemap(const path& rootPath, wgpu::Device device, wgpu::TextureView* pTextureView = nullptr);

	// Load the 16-bit float DFG LUT from some ad-hoc binary format
	static wgpu::Texture loadDFGTexture(const path& path, wgpu::Device device, wgpu::TextureView* pTextureView = nullptr);

private:
	// Compute Tangent and Bitangent attributes from the normal and UVs.
	static void computeTextureFrameAttributes(std::vector<VertexAttributes>& vertexData);
};