struct ComputeInput {
	@builtin(global_invocation_id) id: vec3<u32>,
}

struct Uniforms {
	resolution: u32,
	time: f32,
}

struct Counts {
	point_count: atomic<u32>,
	allocated_vertices: atomic<u32>,
}

struct VertexInput {
	position: vec3<f32>,
	normal: vec3<f32>,
}

struct ModuleLutEntry {
	// Represent a point on an edge of the unit cube
	edge_start_corner: u32,
	edge_end_corner: u32,
};
struct ModuleLut {
	// end_offset[i] is the beginning of the i+1 th entry
	end_offset: array<u32, 256>,
	// Each entry represents a point, to be grouped by 3 to form triangles
	entries: array<ModuleLutEntry>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var distance_grid_write: texture_storage_3d<rgba16float,write>;
@group(0) @binding(2) var distance_grid_read: texture_3d<f32>;
@group(0) @binding(3) var<storage,read_write> counts: Counts;
@group(0) @binding(4) var<storage,read> moduleLut: ModuleLut;
@group(1) @binding(0) var<storage,read_write> vertices: array<VertexInput>;

fn sdBox(p: vec3<f32>, b: vec3<f32>) -> f32 {
	let q = abs(p) - b;
	return length(max(q,vec3<f32>(0.0))) + min(max(q.x,max(q.y,q.z)),0.0);
}

fn opUnion(d1: f32, d2: f32) -> f32 {
	return min(d1, d2);
}

fn opSubtraction(d1: f32, d2: f32) -> f32 {
	return max(d1, -d2);
}

fn evalSdf(pos: vec3<f32>) -> f32 {
	let offset1 = vec3<f32>(0.0);
	let alpha = atan2(pos.y, pos.x);
	let wave = cos(5.0 * alpha - 2.0 * uniforms.time + 10.0 * pos.z);
	let radius1 = 0.55 + 0.05 * wave + 0.1 * cos(3.0 * uniforms.time);
	let d1 = length(pos - offset1) - radius1;

	let offset2 = vec3<f32>(0.0, 0.6, 0.6);
	let radius2 = 0.3;
	let d2 = length(pos - offset2) - radius2;

	let th1 = 0.25 * 3.1415 * uniforms.time;
	let c1 = cos(th1);
	let s1 = sin(th1);
	let th2 = 0.3 * 3.1415;
	let c2 = cos(th2);
	let s2 = sin(th2);
	let M = mat3x3<f32>(
		1.0, 0.0, 0.0,
		0.0, c2, s2,
		0.0, -s2, c2,
	) * mat3x3<f32>(
		c1, s1, 0.0,
		-s1, c1, 0.0,
		0.0, 0.0, 1.0,
	);
	let box_pos = M * (pos - vec3<f32>(0.0, 0.0, 0.8));
	let box = sdBox(box_pos, vec3<f32>(0.15, 0.4, 1.2));

	return opSubtraction(opUnion(d1, d2), box);
}

fn evalNormal(pos: vec3<f32>) -> vec3<f32> {
	const eps = 0.0001;
    const k = vec2<f32>(1.0, -1.0);
    return normalize(k.xyy * evalSdf(pos + k.xyy * eps) + 
                     k.yyx * evalSdf(pos + k.yyx * eps) + 
                     k.yxy * evalSdf(pos + k.yxy * eps) + 
                     k.xxx * evalSdf(pos + k.xxx * eps));
}

fn allocateVertices(vertex_count: u32) -> u32 {
	let addr = atomicAdd(&counts.allocated_vertices, vertex_count);
	return addr;
}

// Transform a corner index into a grid index offset
fn cornerOffset(i: u32) -> vec3<u32> {
	return vec3<u32>(
		(i & (1u << 0)) >> 0,
		(i & (1u << 1)) >> 1,
		(i & (1u << 2)) >> 2,
	);
}
fn cornerOffsetF(i: u32) -> vec3<f32> {
	return vec3<f32>(cornerOffset(i));
}

fn positionFromGridCoord(grid_coord: vec3<f32>) -> vec3<f32> {
	return grid_coord / f32(uniforms.resolution) * 2.0 - 1.0;
}

@compute @workgroup_size(1)
fn main_eval(in: ComputeInput) {
	let position = positionFromGridCoord(vec3<f32>(in.id));
	let d = evalSdf(position);
	textureStore(distance_grid_write, in.id, vec4<f32>(d));
}

@compute @workgroup_size(1)
fn main_reset_count() {
	atomicStore(&counts.point_count, 0);
	atomicStore(&counts.allocated_vertices, 0);
}

@compute @workgroup_size(1)
fn main_count(in: ComputeInput) {
	var cornerDepth: array<f32,8>;
	var module_code: u32 = 0;
	for (var i: u32 = 0 ; i < 8 ; i++) {
		cornerDepth[i] = textureLoad(distance_grid_read, in.id + cornerOffset(i), 0).r;
		if (cornerDepth[i] < 0) {
			module_code += 1u << i;
		}
	}

	var begin_offset = 0u;
	if (module_code > 0) {
		begin_offset = moduleLut.end_offset[module_code - 1];
	}
	let module_point_count = moduleLut.end_offset[module_code] - begin_offset;

	atomicAdd(&counts.point_count, module_point_count);
}

@compute @workgroup_size(1)
fn main_fill(in: ComputeInput) {
	var cornerDepth: array<f32,8>;
	var module_code: u32 = 0;
	for (var i = 0u ; i < 8 ; i++) {
		cornerDepth[i] = textureLoad(distance_grid_read, in.id + cornerOffset(i), 0).r;
		if (cornerDepth[i] < 0) {
			module_code += 1u << i;
		}
	}

	var begin_offset = 0u;
	if (module_code > 0) {
		begin_offset = moduleLut.end_offset[module_code - 1];
	}
	let module_point_count = moduleLut.end_offset[module_code] - begin_offset;

	let addr = allocateVertices(module_point_count);
	for (var i = 0u ; i < module_point_count ; i++) {
		let entry = moduleLut.entries[begin_offset + i];
		let edge_start_corner = cornerOffsetF(entry.edge_start_corner);
		let edge_end_corner = cornerOffsetF(entry.edge_end_corner);
		let start_depth = cornerDepth[entry.edge_start_corner];
		let end_depth = cornerDepth[entry.edge_end_corner];

		let fac = -start_depth / (end_depth - start_depth);
		let grid_offset = edge_start_corner * (1 - fac) + edge_end_corner * fac;
		
		var grid_coord = vec3<f32>(in.id) + grid_offset;
		let position = positionFromGridCoord(grid_coord);
		vertices[addr + i].position = position + vec3<f32>(-1.0, 0.0, 0.0); // global offset for debug
		vertices[addr + i].normal = evalNormal(position);
	}
}