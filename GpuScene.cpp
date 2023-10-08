#include "GpuScene.h"
#include "webgpu-utils.h"
#include "webgpu-gltf-utils.h"

#include "tiny_gltf.h"

#include <glm/gtc/type_ptr.hpp>

#include <cassert>
#include <unordered_map>

using namespace wgpu;
using namespace tinygltf;
using namespace wgpu::gltf; // conversion utils

///////////////////////////////////////////////////////////////////////////////
// Public methods

void GpuScene::createFromModel(wgpu::Device device, const tinygltf::Model& model, BindGroupLayout bindGroupLayout) {
	destroy();

	initDevice(device);
	initBuffers(model);
	initTextures(model);
	initSamplers(model);
	initMaterials(model, bindGroupLayout);
	initDrawCalls(model);
}

void GpuScene::draw(wgpu::RenderPassEncoder renderPass) {
	for (const DrawCall& dc : m_drawCalls) {
		for (size_t layoutIdx = 0; layoutIdx < dc.attributeBufferViews.size(); ++layoutIdx) {
			const auto& view = dc.attributeBufferViews[layoutIdx];
			uint32_t slot = static_cast<uint32_t>(layoutIdx);
			if (view.buffer != -1) {
				renderPass.setVertexBuffer(slot, m_buffers[view.buffer], view.byteOffset, view.byteLength);
			}
			else {
				renderPass.setVertexBuffer(slot, m_nullBuffer, 0, 4 * sizeof(float));
			}
		}
		renderPass.setBindGroup(1, m_materials[dc.materialIndex].bindGroup, 0, nullptr);
		renderPass.setIndexBuffer(m_buffers[dc.indexBufferView.buffer], dc.indexFormat, dc.indexBufferView.byteOffset, dc.indexBufferView.byteLength);
		renderPass.drawIndexed(dc.indexCount, 1, 0, 0, 0);
	}
}

void GpuScene::destroy() {
	terminateDrawCalls();
	terminateMaterials();
	terminateSamplers();
	terminateTextures();
	terminateBuffers();
	terminateDevice();
}

///////////////////////////////////////////////////////////////////////////////
// Private methods

void GpuScene::initDevice(wgpu::Device device) {
	m_device = device;
	m_device.reference();
	m_queue = m_device.getQueue();
}

void GpuScene::terminateDevice() {
	if (m_queue) {
		m_queue.release();
		m_queue = nullptr;
	}
	if (m_device) {
		m_device.release();
		m_device = nullptr;
	}
}

void GpuScene::initBuffers(const tinygltf::Model& model) {
	for (const tinygltf::Buffer& buffer : model.buffers) {
		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = buffer.name.c_str();
		bufferDesc.size = static_cast<uint32_t>(buffer.data.size());
		bufferDesc.usage = BufferUsage::CopyDst | BufferUsage::Vertex | BufferUsage::Index;
		wgpu::Buffer gpuBuffer = m_device.createBuffer(bufferDesc);
		m_buffers.push_back(gpuBuffer);
		m_queue.writeBuffer(gpuBuffer, 0, buffer.data.data(), buffer.data.size());
	}

	{
		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = "Null Buffer";
		bufferDesc.size = static_cast<uint32_t>(4 * sizeof(float));
		bufferDesc.usage = BufferUsage::CopyDst | BufferUsage::Vertex | BufferUsage::Index;
		m_nullBuffer = m_device.createBuffer(bufferDesc);
	}
}

void GpuScene::terminateBuffers() {
	for (wgpu::Buffer b : m_buffers) {
		b.destroy();
		b.release();
	}
	m_buffers.clear();

	if (m_nullBuffer) {
		m_nullBuffer.destroy();
		m_nullBuffer.release();
	}
	m_nullBuffer = nullptr;
}

void GpuScene::initTextures(const tinygltf::Model& model) {
	TextureDescriptor desc;
	for (const tinygltf::Image& image : model.images) {
		// Texture
		desc.label = image.name.c_str();
		desc.dimension = TextureDimension::_2D;
		desc.format = textureFormatToFloatFormat(textureFormatFromGltfImage(image));
		desc.sampleCount = 1;
		desc.size = { static_cast<uint32_t>(image.width) , static_cast<uint32_t>(image.height), 1};
		desc.mipLevelCount = 1;// maxMipLevelCount2D(desc.size); // TODO -> upload mipmaps
		desc.usage = TextureUsage::CopyDst | TextureUsage::TextureBinding;
		desc.viewFormatCount = 0;
		desc.viewFormats = nullptr;
		wgpu::Texture gpuTexture = m_device.createTexture(desc);
		m_textures.push_back(gpuTexture);

		// View
		TextureViewDescriptor viewDesc;
		viewDesc.label = image.name.c_str();
		viewDesc.aspect = TextureAspect::All;
		viewDesc.baseMipLevel = 0;
		viewDesc.mipLevelCount = desc.mipLevelCount;
		viewDesc.baseArrayLayer = 0;
		viewDesc.arrayLayerCount = 1;
		viewDesc.dimension = TextureViewDimension::_2D;
		viewDesc.format = desc.format;
		wgpu::TextureView gpuTextureView = gpuTexture.createView(viewDesc);
		m_textureViews.push_back(gpuTextureView);

		// Upload
		ImageCopyTexture destination;
		destination.aspect = TextureAspect::All;
		destination.mipLevel = 0;
		destination.origin = { 0, 0, 0 };
		destination.texture = gpuTexture;
		TextureDataLayout sourceLayout;
		sourceLayout.offset = 0;
		uint32_t bitsPerPixel = textureFormatBitsPerTexel(desc.format);
		sourceLayout.bytesPerRow = bitsPerPixel * desc.size.width / 8;
		sourceLayout.rowsPerImage = desc.size.height;
		m_queue.writeTexture(destination, image.image.data(), image.image.size(), sourceLayout, desc.size);
	}

	// Default texture
	{
		m_defaultTextureIdx = static_cast<uint32_t>(m_textures.size());

		// Texture
		desc.label = "Default";
		desc.dimension = TextureDimension::_2D;
		desc.format = TextureFormat::RGBA8Unorm;
		desc.sampleCount = 1;
		desc.size = { 1, 1, 1 };
		desc.mipLevelCount = 1;// maxMipLevelCount2D(desc.size); // TODO -> upload mipmaps
		desc.usage = TextureUsage::CopyDst | TextureUsage::TextureBinding;
		desc.viewFormatCount = 0;
		desc.viewFormats = nullptr;
		wgpu::Texture gpuTexture = m_device.createTexture(desc);
		m_textures.push_back(gpuTexture);

		// View
		TextureViewDescriptor viewDesc;
		viewDesc.label = "Default";
		viewDesc.aspect = TextureAspect::All;
		viewDesc.baseMipLevel = 0;
		viewDesc.mipLevelCount = desc.mipLevelCount;
		viewDesc.baseArrayLayer = 0;
		viewDesc.arrayLayerCount = 1;
		viewDesc.dimension = TextureViewDimension::_2D;
		viewDesc.format = desc.format;
		wgpu::TextureView gpuTextureView = gpuTexture.createView(viewDesc);
		m_textureViews.push_back(gpuTextureView);

		// Upload
		ImageCopyTexture destination;
		destination.aspect = TextureAspect::All;
		destination.mipLevel = 0;
		destination.origin = { 0, 0, 0 };
		destination.texture = gpuTexture;
		TextureDataLayout sourceLayout;
		sourceLayout.offset = 0;
		uint32_t bitsPerPixel = textureFormatBitsPerTexel(desc.format);
		sourceLayout.bytesPerRow = bitsPerPixel * desc.size.width / 8;
		sourceLayout.rowsPerImage = desc.size.height;
		uint32_t data = 0;
		m_queue.writeTexture(destination, &data, 4, sourceLayout, desc.size);
	}

	for (const tinygltf::Texture& texture : model.textures) {
		m_sampledTextures.push_back(SampledTexture{
			static_cast<uint32_t>(texture.source),
			static_cast<uint32_t>(texture.sampler),
		});
	}
}

void GpuScene::terminateTextures() {
	m_sampledTextures.clear();
	for (wgpu::TextureView v : m_textureViews) {
		v.release();
	}
	m_textureViews.clear();
	for (wgpu::Texture t : m_textures) {
		t.destroy();
		t.release();
	}
	m_textures.clear();
}

void GpuScene::initSamplers(const tinygltf::Model& model) {
	SamplerDescriptor desc;
	for (const tinygltf::Sampler& sampler : model.samplers) {
		desc.label = sampler.name.c_str();
		desc.magFilter = filterModeFromGltf(sampler.magFilter);
		desc.minFilter = filterModeFromGltf(sampler.minFilter);
		desc.mipmapFilter = mipmapFilterModeFromGltf(sampler.minFilter);
		desc.addressModeU = addressModeFromGltf(sampler.wrapS);
		desc.addressModeV = addressModeFromGltf(sampler.wrapT);
		desc.addressModeW = AddressMode::Repeat;
		desc.lodMinClamp = 0.0;
		desc.lodMaxClamp = 1.0;
		desc.maxAnisotropy = 1.0;
		m_samplers.push_back(m_device.createSampler(desc));
	}

	m_defaultSamplerIdx = static_cast<uint32_t>(m_samplers.size());
	desc.label = "Default";
	m_samplers.push_back(m_device.createSampler(desc));
}

void GpuScene::terminateSamplers() {
	for (wgpu::Sampler s : m_samplers) {
		s.release();
	}
	m_samplers.clear();
}

void GpuScene::initMaterials(const tinygltf::Model& model, BindGroupLayout bindGroupLayout) {
	for (const tinygltf::Material& material : model.materials) {
		GpuScene::Material gpuMaterial;

		TextureSampleType baseColorSampleType = TextureSampleType::Undefined;
		int baseColorSampledTextureIdx = material.pbrMetallicRoughness.baseColorTexture.index;
		int baseColorTextureIdx = -1;
		int baseColorSamplerIdx = -1;
		if (baseColorSampledTextureIdx >= 0) {
			baseColorTextureIdx = m_sampledTextures[baseColorSampledTextureIdx].textureIndex;
			baseColorSamplerIdx = m_sampledTextures[baseColorSampledTextureIdx].samplerIndex;
			baseColorSampleType = textureFormatSupportedSampleType(m_textures[baseColorTextureIdx].getFormat());
		}

		// Uniforms
		BufferDescriptor bufferDesc;
		bufferDesc.label = material.name.c_str();
		bufferDesc.mappedAtCreation = false;
		bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(MaterialUniforms);
		gpuMaterial.uniformBuffer = m_device.createBuffer(bufferDesc);

		// Uniform Values
		gpuMaterial.uniforms.baseColorFactor = glm::make_vec4(material.pbrMetallicRoughness.baseColorFactor.data());
		gpuMaterial.uniforms.metallicFactor = material.pbrMetallicRoughness.metallicFactor;
		gpuMaterial.uniforms.roughnessFactor = material.pbrMetallicRoughness.roughnessFactor;
		if (baseColorTextureIdx >= 0) {
			gpuMaterial.uniforms.baseColorTexCoords = static_cast<uint32_t>(material.pbrMetallicRoughness.baseColorTexture.texCoord);
		}
		else {
			gpuMaterial.uniforms.baseColorTexCoords = WGPU_LIMIT_U32_UNDEFINED;
		}
		m_queue.writeBuffer(gpuMaterial.uniformBuffer, 0, &gpuMaterial.uniforms, sizeof(MaterialUniforms));

		// Bind Group
		std::vector<BindGroupEntry> bindGroupEntries(3, Default);
		bindGroupEntries[0].binding = 0;
		bindGroupEntries[0].buffer = gpuMaterial.uniformBuffer;
		bindGroupEntries[0].size = sizeof(MaterialUniforms);

		bindGroupEntries[1].binding = 1;
		int idx = baseColorTextureIdx >= 0 ? baseColorTextureIdx : m_defaultTextureIdx;
		bindGroupEntries[1].textureView = m_textureViews[idx];

		bindGroupEntries[2].binding = 2;
		idx = baseColorSamplerIdx >= 0 ? baseColorSamplerIdx : m_defaultSamplerIdx;
		bindGroupEntries[2].sampler = m_samplers[idx];

		BindGroupDescriptor bindGroupDesc;
		bindGroupDesc.label = material.name.c_str();
		bindGroupDesc.entryCount = static_cast<uint32_t>(bindGroupEntries.size());
		bindGroupDesc.entries = bindGroupEntries.data();
		bindGroupDesc.layout = bindGroupLayout;
		gpuMaterial.bindGroup = m_device.createBindGroup(bindGroupDesc);

		m_materials.push_back(gpuMaterial);
	}

	// Add default material
	{
		GpuScene::Material gpuMaterial;
		m_defaultMaterialIdx = static_cast<uint32_t>(m_materials.size());

		// Uniforms
		BufferDescriptor bufferDesc;
		bufferDesc.label = "Default Material";
		bufferDesc.mappedAtCreation = false;
		bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(MaterialUniforms);
		gpuMaterial.uniformBuffer = m_device.createBuffer(bufferDesc);

		// Uniform Values
		gpuMaterial.uniforms.baseColorFactor = {1.0, 0.5, 0.5, 1.0};
		gpuMaterial.uniforms.metallicFactor = 0.0;
		gpuMaterial.uniforms.roughnessFactor = 0.2;
		gpuMaterial.uniforms.baseColorTexCoords = WGPU_LIMIT_U32_UNDEFINED;
		m_queue.writeBuffer(gpuMaterial.uniformBuffer, 0, &gpuMaterial.uniforms, sizeof(MaterialUniforms));

		// Bind Group
		std::vector<BindGroupEntry> bindGroupEntries(3, Default);
		bindGroupEntries[0].binding = 0;
		bindGroupEntries[0].buffer = gpuMaterial.uniformBuffer;
		bindGroupEntries[0].size = sizeof(MaterialUniforms);

		bindGroupEntries[1].binding = 1;
		bindGroupEntries[1].textureView = m_textureViews[m_defaultTextureIdx];

		bindGroupEntries[2].binding = 2;
		bindGroupEntries[2].sampler = m_samplers[m_defaultSamplerIdx];

		BindGroupDescriptor bindGroupDesc;
		bindGroupDesc.label = "Default Material";
		bindGroupDesc.entryCount = static_cast<uint32_t>(bindGroupEntries.size());
		bindGroupDesc.entries = bindGroupEntries.data();
		bindGroupDesc.layout = bindGroupLayout;
		gpuMaterial.bindGroup = m_device.createBindGroup(bindGroupDesc);

		m_materials.push_back(gpuMaterial);
	}
}

void GpuScene::terminateMaterials() {
	for (Material& mat : m_materials) {
		mat.bindGroup.release();
		mat.uniformBuffer.destroy();
		mat.uniformBuffer.release();
	}
	m_materials.clear();
}

void GpuScene::initDrawCalls(const tinygltf::Model& model) {
	const Mesh& mesh = model.meshes[0];
	const Primitive& prim = mesh.primitives[0];

	std::vector<std::tuple<std::string, uint32_t, VertexFormat>> semanticToLocation = {
		// GLTF Semantic, Shader input location, Default format
		{"POSITION", 0, VertexFormat::Float32x3},
		{"NORMAL", 1, VertexFormat::Float32x3},
		{"COLOR", 2, VertexFormat::Float32x3},
		{"TEXCOORD_0", 3, VertexFormat::Float32x2},
	};

	// We create one vertex buffer layout per bufferView
	std::vector<VertexBufferLayout> vertexBufferLayouts;
	// For each layout, there are multiple attributes
	std::vector<std::vector<VertexAttribute>> vertexBufferLayoutToAttributes;
	// For each layout, there is a single buffer view
	std::vector<tinygltf::BufferView> vertexBufferLayoutToBufferView;
	// And we keep the map from bufferView to layout index
	// NB: The index '-1' is used for attributes expected by the shader but not
	// provided by the GLTF file.
	std::unordered_map<int, size_t> bufferViewToVertexBufferLayout;

	for (const auto& [attrSemantic, location, defaultFormat] : semanticToLocation) {
		int bufferViewIdx;
		BufferView bufferView = {};
		VertexFormat format = VertexFormat::Undefined;
		uint64_t byteOffset;

		auto accessorIt = prim.attributes.find(attrSemantic);
		if (accessorIt != prim.attributes.end()) {
			Accessor accessor = model.accessors[accessorIt->second];
			bufferViewIdx = accessor.bufferView;
			bufferView = model.bufferViews[bufferViewIdx];
			format = vertexFormatFromAccessor(accessor);
			byteOffset = static_cast<uint64_t>(accessor.byteOffset);
			if (bufferView.byteStride == 0) {
				bufferView.byteStride = vertexFormatByteSize(format);
			}
		}
		else {
			bufferViewIdx = -1;
			bufferView.buffer = -1;
			format = defaultFormat;
			byteOffset = 0;
		}

		// Group attributes by bufferView
		size_t layoutIdx = 0;
		auto vertexBufferLayoutIt = bufferViewToVertexBufferLayout.find(bufferViewIdx);
		if (vertexBufferLayoutIt == bufferViewToVertexBufferLayout.end()) {
			layoutIdx = vertexBufferLayouts.size();
			VertexBufferLayout layout;
			layout.arrayStride = static_cast<uint64_t>(bufferView.byteStride);
			layout.stepMode = VertexStepMode::Vertex;
			vertexBufferLayouts.push_back(layout);
			vertexBufferLayoutToAttributes.push_back({});
			vertexBufferLayoutToBufferView.push_back(bufferView);
			bufferViewToVertexBufferLayout[bufferViewIdx] = layoutIdx;
		}
		else {
			layoutIdx = vertexBufferLayoutIt->second;
		}

		VertexAttribute attrib;
		attrib.shaderLocation = location;
		attrib.format = format;
		attrib.offset = byteOffset;
		vertexBufferLayoutToAttributes[layoutIdx].push_back(attrib);
	}

	// Index data
	const Accessor& indexAccessor = model.accessors[prim.indices];
	const BufferView& indexBufferView = model.bufferViews[indexAccessor.bufferView];
	IndexFormat indexFormat = indexFormatFromAccessor(indexAccessor);
	assert(indexFormat != IndexFormat::Undefined);
	assert(indexAccessor.type == TINYGLTF_TYPE_SCALAR);

	m_drawCalls.push_back(DrawCall{
		vertexBufferLayoutToAttributes,
		vertexBufferLayouts,
		vertexBufferLayoutToBufferView,
		indexBufferView,
		indexFormat,
		static_cast<uint32_t>(indexAccessor.count),
		prim.material >= 0 ? static_cast<uint32_t>(prim.material) : m_defaultMaterialIdx, // TODO: create a default material
	});
}

void GpuScene::terminateDrawCalls() {
	m_drawCalls.clear();
}

std::vector<wgpu::VertexBufferLayout> GpuScene::vertexBufferLayouts(uint32_t drawCallIndex) const {
	const auto& dc = m_drawCalls[drawCallIndex];
	std::vector<wgpu::VertexBufferLayout> vertexBufferLayouts = dc.vertexBufferLayouts;

	// Assign pointer addresses (should be done upon foreign access)
	for (size_t layoutIdx = 0; layoutIdx < dc.vertexBufferLayouts.size(); ++layoutIdx) {
		VertexBufferLayout& layout = vertexBufferLayouts[layoutIdx];
		const auto& attributes = dc.vertexAttributes[layoutIdx];
		layout.attributeCount = static_cast<uint32_t>(attributes.size());
		layout.attributes = attributes.data();
	}

	return vertexBufferLayouts;
}