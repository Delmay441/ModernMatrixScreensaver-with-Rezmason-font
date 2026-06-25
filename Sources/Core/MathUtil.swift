import simd

// Column-major 4x4 matrix helpers for a right-handed camera with a
// Metal-style [0, 1] depth range. Vectors are treated as columns: v' = M * v.

@inline(__always) func radians(_ deg: Float) -> Float { deg * .pi / 180 }

enum Mat {
    static let identity = matrix_identity_float4x4

    static func translation(_ t: SIMD3<Float>) -> float4x4 {
        float4x4(columns: (
            SIMD4(1, 0, 0, 0),
            SIMD4(0, 1, 0, 0),
            SIMD4(0, 0, 1, 0),
            SIMD4(t.x, t.y, t.z, 1)
        ))
    }

    static func scale(_ s: SIMD3<Float>) -> float4x4 {
        float4x4(columns: (
            SIMD4(s.x, 0, 0, 0),
            SIMD4(0, s.y, 0, 0),
            SIMD4(0, 0, s.z, 0),
            SIMD4(0, 0, 0, 1)
        ))
    }

    static func rotation(angle: Float, axis: SIMD3<Float>) -> float4x4 {
        let a = normalize(axis)
        let c = cosf(angle), s = sinf(angle), ic = 1 - c
        let x = a.x, y = a.y, z = a.z
        return float4x4(columns: (
            SIMD4(c + x*x*ic,     y*x*ic + z*s, z*x*ic - y*s, 0),
            SIMD4(x*y*ic - z*s,   c + y*y*ic,   z*y*ic + x*s, 0),
            SIMD4(x*z*ic + y*s,   y*z*ic - x*s, c + z*z*ic,   0),
            SIMD4(0, 0, 0, 1)
        ))
    }

    /// Right-handed perspective, depth mapped to [0, 1] (Metal/D3D convention).
    static func perspective(fovyRadians fovy: Float, aspect: Float, near: Float, far: Float) -> float4x4 {
        let f = 1 / tanf(fovy * 0.5)
        return float4x4(columns: (
            SIMD4(f / aspect, 0, 0, 0),
            SIMD4(0, f, 0, 0),
            SIMD4(0, 0, far / (near - far), -1),
            SIMD4(0, 0, (far * near) / (near - far), 0)
        ))
    }

    /// Right-handed look-at: camera at `eye` looking toward `center` with `up`.
    static func lookAt(eye: SIMD3<Float>, center: SIMD3<Float>, up: SIMD3<Float>) -> float4x4 {
        let f = normalize(center - eye)
        let s = normalize(cross(f, up))
        let u = cross(s, f)
        return float4x4(columns: (
            SIMD4(s.x, u.x, -f.x, 0),
            SIMD4(s.y, u.y, -f.y, 0),
            SIMD4(s.z, u.z, -f.z, 0),
            SIMD4(-dot(s, eye), -dot(u, eye), dot(f, eye), 1)
        ))
    }
}

@inline(__always) func smoothstep(_ a: Float, _ b: Float, _ x: Float) -> Float {
    let t = simd_clamp((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)
}

@inline(__always) func lerp(_ a: Float, _ b: Float, _ t: Float) -> Float { a + (b - a) * t }
