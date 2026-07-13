import Metal
import QuartzCore
import CoreGraphics
import simd

struct Camera {
    // Eye and centre share Y so the view is dead-level (no downward pitch / "top tilt").
    // Depth is preserved by the Z spread of the rain, not by tilting the camera.
    var eye = SIMD3<Float>(0, 0, 48)
    var center = SIMD3<Float>(0, 0, -8)
    var up = SIMD3<Float>(0, 1, 0)
    var fovy: Float = radians(46)
    var aspect: Float = 1
    var near: Float = 1
    var far: Float = 240

    struct Resolved { var viewProj: float4x4; var eye, right, up: SIMD3<Float> }

    func resolve() -> Resolved {
        let view = Mat.lookAt(eye: eye, center: center, up: up)
        let proj = Mat.perspective(fovyRadians: fovy, aspect: aspect, near: near, far: far)
        let forward = normalize(center - eye)
        let right = normalize(cross(forward, up))
        let u = cross(right, forward)
        return Resolved(viewProj: proj * view, eye: eye, right: right, up: u)
    }
}

/// Slowly drifts the camera between a set of framed "nice views", à la GLMatrix.
struct PanController {
    // (eye, center) presets, all keeping the rain volume framed.
    let presets: [(SIMD3<Float>, SIMD3<Float>)] = [
        (SIMD3(0, 3, 48),   SIMD3(0, 0, -8)),
        (SIMD3(15, 7, 45),  SIMD3(-3, 0, -8)),
        (SIMD3(0, 15, 43),  SIMD3(0, -3, -10)),
        (SIMD3(-15, 6, 45), SIMD3(3, 0, -8)),
        (SIMD3(0, -5, 50),  SIMD3(0, 4, -6)),
        (SIMD3(9, 2, 38),   SIMD3(-4, 0, -10)),
    ]
    var segment: Float = 9     // seconds per move+dwell

    func apply(to cam: inout Camera, time: Float) {
        let n = presets.count
        let cycle = time / segment
        let i = Int(floor(cycle)) % n
        let j = (i + 1) % n
        let f = cycle - floor(cycle)
        let ease = smoothstep(0, 1, min(1, f / 0.45))   // move first 45%, then dwell
        cam.eye = lerp3(presets[i].0, presets[j].0, ease)
        cam.center = lerp3(presets[i].1, presets[j].1, ease)
    }
}

@inline(__always) func lerp3(_ a: SIMD3<Float>, _ b: SIMD3<Float>, _ t: Float) -> SIMD3<Float> {
    a + (b - a) * t
}

/// Owns the Metal device/pipelines and renders the rain with optional bloom + HDR.
/// Drives either a live CAMetalLayer (display-link path) or an offscreen texture.
final class Renderer {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let library: MTLLibrary
    private let sampler: MTLSamplerState

    private var glyphPipelines: [UInt: MTLRenderPipelineState] = [:]
    private var compositePipelines: [UInt: MTLRenderPipelineState] = [:]
    private var thresholdPipeline: MTLRenderPipelineState!
    private var blurPipeline: MTLRenderPipelineState!

    private var atlas: GlyphAtlas
    private let sim: OpaquePointer        // mmcore MMSim* (shared C engine)
    private(set) var settings: Settings

    private let maxInstances = 65_536
    private let frameCount = 3
    private var instanceBuffers: [MTLBuffer] = []
    private var uniformBuffers: [MTLBuffer] = []
    private var frameIndex = 0
    private let inflight = DispatchSemaphore(value: 3)

    // Post-processing intermediates.
    private var sceneTex: MTLTexture?
    private var bloomA: MTLTexture?
    private var bloomB: MTLTexture?
    private var postSize = CGSize.zero

    var camera = Camera()
    private var pan = PanController()
    private var panTime: Float = 0
    var viewportSize = CGSize(width: 1, height: 1)
    private var time: Float = 0
    private(set) var fps: Double = 0

    private let sceneFormat: MTLPixelFormat = .rgba16Float

    init?(device: MTLDevice, bundle: Bundle, settings: Settings) {
        guard let queue = device.makeCommandQueue(),
              let library = try? device.makeDefaultLibrary(bundle: bundle) else { return nil }
        self.device = device
        self.queue = queue
        self.library = library
        self.settings = settings
        self.atlas = GlyphAtlas(device: device, encoding: settings.encoding)
        var mset = settings.mm
        self.sim = mm_sim_create(&mset, Int32(atlas.glyphCount), UInt64.random(in: 1...UInt64.max))

        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear; sd.magFilter = .linear; sd.mipFilter = .linear
        sd.sAddressMode = .clampToEdge; sd.tAddressMode = .clampToEdge
        guard let samp = device.makeSamplerState(descriptor: sd) else { return nil }
        self.sampler = samp

        for _ in 0..<frameCount {
            guard let ib = device.makeBuffer(length: maxInstances * MemoryLayout<MMGlyphInstance>.stride,
                                             options: .storageModeShared),
                  let ub = device.makeBuffer(length: MemoryLayout<Uniforms>.stride,
                                             options: .storageModeShared) else { return nil }
            instanceBuffers.append(ib); uniformBuffers.append(ub)
        }

        thresholdPipeline = makeFSPipeline(fragment: "bloom_threshold", format: sceneFormat)
        blurPipeline = makeFSPipeline(fragment: "bloom_blur", format: sceneFormat)
    }

    deinit { mm_sim_destroy(sim) }

    // MARK: Settings + layer configuration

    func apply(_ new: Settings) {
        if new.encoding != settings.encoding {
            atlas = GlyphAtlas(device: device, encoding: new.encoding)
        }
        var mset = new.mm
        mm_sim_update(sim, &mset, Int32(atlas.glyphCount))
        settings = new
    }

    /// Configures a live layer's pixel format / EDR according to the HDR setting.
    func configure(layer: CAMetalLayer) {
        if settings.hdr {
            layer.pixelFormat = .rgba16Float
            layer.wantsExtendedDynamicRangeContent = true
            layer.colorspace = CGColorSpace(name: CGColorSpace.extendedLinearDisplayP3)
        } else {
            layer.pixelFormat = .bgra8Unorm
            layer.wantsExtendedDynamicRangeContent = false
            layer.colorspace = CGColorSpace(name: CGColorSpace.displayP3)
        }
        layer.framebufferOnly = true    // we only render into the drawable, never read it
    }

    // MARK: Pipelines

    private func makeFSPipeline(fragment: String, format: MTLPixelFormat) -> MTLRenderPipelineState {
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = library.makeFunction(name: "fullscreen_vertex")
        d.fragmentFunction = library.makeFunction(name: fragment)
        d.colorAttachments[0].pixelFormat = format
        return try! device.makeRenderPipelineState(descriptor: d)
    }

    private func glyphPipeline(for format: MTLPixelFormat) -> MTLRenderPipelineState {
        if let p = glyphPipelines[format.rawValue] { return p }
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = library.makeFunction(name: "glyph_vertex")
        d.fragmentFunction = library.makeFunction(name: "glyph_fragment")
        let c = d.colorAttachments[0]!
        c.pixelFormat = format
        c.isBlendingEnabled = true
        c.rgbBlendOperation = .add
        c.alphaBlendOperation = .add
        c.sourceRGBBlendFactor = .one
        c.sourceAlphaBlendFactor = .one
        c.destinationRGBBlendFactor = .oneMinusSourceAlpha
        c.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        let p = try! device.makeRenderPipelineState(descriptor: d)
        glyphPipelines[format.rawValue] = p
        return p
    }

    private func compositePipeline(for format: MTLPixelFormat) -> MTLRenderPipelineState {
        if let p = compositePipelines[format.rawValue] { return p }
        let p = makeFSPipeline(fragment: "bloom_composite", format: format)
        compositePipelines[format.rawValue] = p
        return p
    }

    // MARK: Texture management

    private func ensurePostTextures(width: Int, height: Int) {
        let size = CGSize(width: width, height: height)
        if size == postSize, sceneTex != nil { return }
        postSize = size
        sceneTex = makeTarget(width: width, height: height)
        bloomA = makeTarget(width: max(1, width / 2), height: max(1, height / 2))
        bloomB = makeTarget(width: max(1, width / 2), height: max(1, height / 2))
    }

    private func makeTarget(width: Int, height: Int) -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: sceneFormat, width: width, height: height, mipmapped: false)
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        return device.makeTexture(descriptor: d)!
    }

    // MARK: Uniforms

    private func makeUniforms() -> Uniforms {
        var cam = camera
        if settings.panning { pan.apply(to: &cam, time: panTime) }
        cam.aspect = Float(max(viewportSize.width, 1) / max(viewportSize.height, 1))
        let r = cam.resolve()
        var u = Uniforms()
        u.viewProj = r.viewProj
        u.camX = r.eye.x; u.camY = r.eye.y; u.camZ = r.eye.z
        u.camRX = r.right.x; u.camRY = r.right.y; u.camRZ = r.right.z
        u.camUX = r.up.x; u.camUY = r.up.y; u.camUZ = r.up.z
        u.glyphHalf = 0.70
        u.atlasCols = Float(atlas.cols); u.atlasRows = Float(atlas.rows)
        u.time = time
        u.fogEnabled = settings.fog ? 1 : 0
        u.fogStartDist = 46; u.fogEndDist = 112
        u.textured = settings.textured ? 1 : 0
        u.wireframe = settings.wireframe ? 1 : 0
        return u
    }

    // MARK: Scene + post encoding

    private func encodeScene(into cb: MTLCommandBuffer, slot: Int) {
        guard let sceneTex else { return }
        let ib = instanceBuffers[slot]
        let ptr = ib.contents().bindMemory(to: MMGlyphInstance.self, capacity: maxInstances)
        let count = Int(mm_sim_write_instances(sim, ptr, Int32(maxInstances)))
        var u = makeUniforms()
        memcpy(uniformBuffers[slot].contents(), &u, MemoryLayout<Uniforms>.stride)

        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = sceneTex
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        guard let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { return }
        enc.setRenderPipelineState(glyphPipeline(for: sceneFormat))
        enc.setVertexBuffer(ib, offset: 0, index: 0)
        enc.setVertexBuffer(uniformBuffers[slot], offset: 0, index: 1)
        enc.setFragmentBuffer(uniformBuffers[slot], offset: 0, index: 1)
        enc.setFragmentTexture(atlas.texture, index: 0)
        enc.setFragmentSamplerState(sampler, index: 0)
        if count > 0 {
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: count)
        }
        enc.endEncoding()
    }

    private func fsPass(_ cb: MTLCommandBuffer, pipeline: MTLRenderPipelineState,
                       target: MTLTexture, inputs: [MTLTexture],
                       setup: ((MTLRenderCommandEncoder) -> Void)? = nil) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .dontCare
        pass.colorAttachments[0].storeAction = .store
        guard let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { return }
        enc.setRenderPipelineState(pipeline)
        for (i, t) in inputs.enumerated() { enc.setFragmentTexture(t, index: i) }
        enc.setFragmentSamplerState(sampler, index: 0)
        setup?(enc)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: 1)
        enc.endEncoding()
    }

    private func encodePost(_ cb: MTLCommandBuffer, finalTex: MTLTexture) {
        guard let sceneTex else { return }
        let composite = compositePipeline(for: finalTex.pixelFormat)

        if settings.bloom, let a = bloomA, let b = bloomB {
            var knee: Float = 0.72
            fsPass(cb, pipeline: thresholdPipeline, target: a, inputs: [sceneTex]) { enc in
                enc.setFragmentBytes(&knee, length: 4, index: 0)
            }
            let texel = SIMD2<Float>(1.0 / Float(a.width), 1.0 / Float(a.height))
            for _ in 0..<2 {
                var hx = SIMD2<Float>(texel.x, 0)
                fsPass(cb, pipeline: blurPipeline, target: b, inputs: [a]) { $0.setFragmentBytes(&hx, length: 8, index: 0) }
                var vy = SIMD2<Float>(0, texel.y)
                fsPass(cb, pipeline: blurPipeline, target: a, inputs: [b]) { $0.setFragmentBytes(&vy, length: 8, index: 0) }
            }
            var intensity = Float(settings.bloomIntensity) * 1.6
            fsPass(cb, pipeline: composite, target: finalTex, inputs: [sceneTex, a]) { enc in
                enc.setFragmentBytes(&intensity, length: 4, index: 0)
            }
        } else {
            var intensity: Float = 0
            fsPass(cb, pipeline: composite, target: finalTex, inputs: [sceneTex, sceneTex]) { enc in
                enc.setFragmentBytes(&intensity, length: 4, index: 0)
            }
        }
    }

    // MARK: Live path

    func draw(in layer: CAMetalLayer, dt: Float) {
        guard let drawable = layer.nextDrawable() else { return }
        ensurePostTextures(width: drawable.texture.width, height: drawable.texture.height)
        inflight.wait()
        let slot = frameIndex
        frameIndex = (frameIndex + 1) % frameCount
        time += dt; panTime += dt
        mm_sim_advance(sim, dt)
        if dt > 0 { let inst = 1.0 / Double(dt); fps = fps == 0 ? inst : fps * 0.9 + inst * 0.1 }

        guard let cb = queue.makeCommandBuffer() else { inflight.signal(); return }
        cb.addCompletedHandler { [weak self] _ in self?.inflight.signal() }
        encodeScene(into: cb, slot: slot)
        encodePost(cb, finalTex: drawable.texture)
        cb.present(drawable)
        cb.commit()
    }

    // MARK: Snapshot path (offscreen, synchronous, LDR)

    func renderImage(width: Int, height: Int, warmupFrames: Int = 90, dt: Float = 1.0 / 60) -> CGImage? {
        viewportSize = CGSize(width: width, height: height)
        ensurePostTextures(width: width, height: height)
        for _ in 0..<warmupFrames { time += dt; panTime += dt; mm_sim_advance(sim, dt) }

        let desc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        desc.usage = [.renderTarget, .shaderRead]
        desc.storageMode = .shared
        guard let target = device.makeTexture(descriptor: desc),
              let cb = queue.makeCommandBuffer() else { return nil }
        encodeScene(into: cb, slot: 0)
        encodePost(cb, finalTex: target)
        cb.commit()
        cb.waitUntilCompleted()
        return Renderer.cgImage(from: target)
    }

    private static func cgImage(from tex: MTLTexture) -> CGImage? {
        let w = tex.width, h = tex.height
        let rowBytes = w * 4
        var data = [UInt8](repeating: 0, count: rowBytes * h)
        tex.getBytes(&data, bytesPerRow: rowBytes, from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
        let cs = CGColorSpaceCreateDeviceRGB()
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue
                                | CGBitmapInfo.byteOrder32Little.rawValue)
        guard let ctx = CGContext(data: &data, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: rowBytes, space: cs, bitmapInfo: info.rawValue) else { return nil }
        return ctx.makeImage()
    }
}
