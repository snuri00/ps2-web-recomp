struct Blit {
    width: u32,
    height: u32,
    current: u32,
    previous: u32,
    blend: u32,
    out_width: u32,
    out_height: u32,
    shader_mode: u32,
}

@group(0) @binding(0) var<storage, read> pixels: array<u32>;
@group(0) @binding(1) var<uniform> blit: Blit;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs(@builtin(vertex_index) i: u32) -> VertexOut {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VertexOut;
    o.position = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2<f32>(p.x, 1.0 - p.y);
    return o;
}

fn texel(base: u32, x: i32, y: i32) -> vec4<f32> {
    let cx = u32(clamp(x, 0, i32(blit.width) - 1));
    let cy = u32(clamp(y, 0, i32(blit.height) - 1));
    return unpack4x8unorm(pixels[base + cy * blit.width + cx]);
}

fn sample(base: u32, uv: vec2<f32>) -> vec4<f32> {
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    let scale = max(floor(vec2<f32>(f32(blit.out_width), f32(blit.out_height)) / size), vec2<f32>(1.0));
    let t = uv * size;
    let d = fract(t) - 0.5;
    let region = 0.5 - 0.5 / scale;
    let p = floor(t) + (d - clamp(d, -region, region)) * scale;
    let origin = floor(p);
    let f = p - origin;
    let x = i32(origin.x);
    let y = i32(origin.y);
    let top = mix(texel(base, x, y), texel(base, x + 1, y), f.x);
    let bottom = mix(texel(base, x, y + 1), texel(base, x + 1, y + 1), f.x);
    return mix(top, bottom, f.y);
}

fn sample_bilinear(base: u32, uv: vec2<f32>) -> vec4<f32> {
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    let t = uv * size - vec2<f32>(0.5, 0.5);
    let origin = floor(t);
    let f = fract(t);
    let x = i32(origin.x);
    let y = i32(origin.y);
    let c00 = texel(base, x, y);
    let c10 = texel(base, x + 1, y);
    let c01 = texel(base, x, y + 1);
    let c11 = texel(base, x + 1, y + 1);
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4<f32> {
    var c: vec4<f32>;
    if (blit.shader_mode == 1u) {
        c = sample_bilinear(blit.current, in.uv);
        if (blit.blend != 0u) {
            c = (c + sample_bilinear(blit.previous, in.uv)) * 0.5;
        }
    } else {
        c = sample(blit.current, in.uv);
        if (blit.blend != 0u) {
            c = (c + sample(blit.previous, in.uv)) * 0.5;
        }
    }

    if (blit.shader_mode == 2u) {
        let scan = sin(in.uv.y * f32(blit.height) * 3.14159265);
        let factor = 0.82 + 0.18 * scan * scan;
        c = vec4<f32>(c.rgb * factor, c.a);
    } else if (blit.shader_mode == 3u) {
        let lum = dot(c.rgb, vec3<f32>(0.299, 0.587, 0.114));
        let sat = mix(vec3<f32>(lum), c.rgb, 1.25);
        c = vec4<f32>(clamp(sat * 1.05, vec3<f32>(0.0), vec3<f32>(1.0)), c.a);
    }

    return vec4<f32>(c.rgb, 1.0);
}
