struct Blit {
    width: u32,
    height: u32,
    current: u32,
    previous: u32,
    blend: u32,
    out_width: u32,
    out_height: u32,
    options: u32,
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

fn scene(uv: vec2<f32>) -> vec3<f32> {
    var c = sample(blit.current, uv).rgb;
    if (blit.blend != 0u) { c = (c + sample(blit.previous, uv).rgb) * 0.5; }
    return c;
}

fn luma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.299, 0.587, 0.114)); }

fn edge_aa(uv: vec2<f32>, center: vec3<f32>) -> vec3<f32> {
    // Fast approximate edge AA, evaluated in image space. Flat regions return
    // unchanged, while edge direction controls subpixel reconstruction.
    let texel_step = 1.0 / vec2<f32>(f32(blit.width), f32(blit.height));
    let nw = luma(scene(uv + texel_step * vec2<f32>(-1.0, -1.0)));
    let ne = luma(scene(uv + texel_step * vec2<f32>(1.0, -1.0)));
    let sw = luma(scene(uv + texel_step * vec2<f32>(-1.0, 1.0)));
    let se = luma(scene(uv + texel_step * vec2<f32>(1.0, 1.0)));
    let m = luma(center);
    let low = min(m, min(min(nw, ne), min(sw, se)));
    let high = max(m, max(max(nw, ne), max(sw, se)));
    if (high - low < max(0.0312, high * 0.125)) { return center; }
    var direction = vec2<f32>(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    let reduce = max((nw + ne + sw + se) * 0.03125, 0.0078125);
    direction = clamp(direction / (min(abs(direction.x), abs(direction.y)) + reduce), vec2<f32>(-8.0), vec2<f32>(8.0)) * texel_step;
    let a = 0.5 * (scene(uv - direction / 6.0) + scene(uv + direction / 6.0));
    let b = a * 0.5 + 0.25 * (scene(uv - direction * 0.5) + scene(uv + direction * 0.5));
    let lb = luma(b);
    return select(b, a, lb < low || lb > high);
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4<f32> {
    var c = scene(in.uv);
    if ((blit.options & 1u) != 0u) { c = edge_aa(in.uv, c); }
    return vec4<f32>(clamp(c, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
