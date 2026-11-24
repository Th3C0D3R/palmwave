//! XCL's small text pass. Resources are reused only after the presenter's existing fence.

use std::ffi::c_void;
use std::mem::ManuallyDrop;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;
use windows::Win32::Foundation::RECT;
use windows::Win32::Graphics::Direct3D::*;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;

use super::overlay_font as font;

const COLUMNS: usize = 64;
const ROWS: usize = 10;
static VISIBLE: AtomicBool = AtomicBool::new(true);

pub(super) fn enabled() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var("XCL_OVERLAY").as_deref() == Ok("1"))
}

pub(super) fn visible() -> bool {
    enabled() && VISIBLE.load(Ordering::Relaxed)
}

pub(super) fn key_message(msg: u32, key: usize, flags: isize) -> bool {
    enabled() && toggle_key(&VISIBLE, msg, key, flags)
}

fn toggle_key(visible: &AtomicBool, msg: u32, key: usize, flags: isize) -> bool {
    use windows::Win32::UI::WindowsAndMessaging::*;
    if key != 0x79 { return false; } // VK_F10, unused by the gamepad mapping
    match msg {
        WM_KEYDOWN | WM_SYSKEYDOWN => {
            if flags & (1 << 30) == 0 { visible.fetch_xor(true, Ordering::Relaxed); }
            true
        }
        WM_KEYUP | WM_SYSKEYUP => true,
        _ => false,
    }
}

const SHADER: &str = r#"
cbuffer Layout : register(b0) { float2 viewport; float scale; uint columns; };
StructuredBuffer<uint> cells : register(t1);
Texture2D<float> atlas : register(t0);
struct Vertex { float4 position : SV_Position; float2 pixel : TEXCOORD0; nointerpolation uint glyph : TEXCOORD1; };
Vertex vs(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = { float2(0,0), float2(1,0), float2(0,1), float2(0,1), float2(1,0), float2(1,1) };
    float2 corner = corners[vertex];
    float2 p = 12 + (float2(instance % columns, instance / columns) + corner) * (8 * scale);
    Vertex o;
    o.position = float4(p / viewport * float2(2,-2) + float2(-1,1), 0, 1);
    o.pixel = corner * 8;
    o.glyph = cells[instance];
    return o;
}
float4 ps(Vertex input) : SV_Target {
    uint2 p = uint2(input.glyph % 16, input.glyph / 16) * 8 + min(uint2(input.pixel), 7);
    float ink = atlas.Load(int3(p, 0));
    return ink > 0.5 ? float4(0.75, 1, 0.85, 1) : float4(0, 0, 0, 0.8);
}
"#;

pub(super) struct Overlay {
    root: ID3D12RootSignature,
    pso: ID3D12PipelineState,
    srv: ID3D12DescriptorHeap,
    rtv: ID3D12DescriptorHeap,
    atlas: ID3D12Resource,
    upload: ID3D12Resource,
    cells: ID3D12Resource,
    uploaded: bool,
}

pub(super) unsafe fn buffer(dev: &ID3D12Device, bytes: u64, heap: D3D12_HEAP_TYPE, state: D3D12_RESOURCE_STATES) -> windows::core::Result<ID3D12Resource> {
    let mut out = None;
    dev.CreateCommittedResource(
        &D3D12_HEAP_PROPERTIES { Type: heap, ..Default::default() }, D3D12_HEAP_FLAG_NONE,
        &D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER, Width: bytes, Height: 1,
            DepthOrArraySize: 1, MipLevels: 1, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR, ..Default::default()
        }, state, None, &mut out,
    )?;
    out.ok_or_else(|| windows::core::Error::from_hresult(windows::Win32::Foundation::E_FAIL))
}

impl Overlay {
    pub(super) fn submitted(&mut self) { self.uploaded = true; }

    pub(super) unsafe fn new(dev: &ID3D12Device, format: DXGI_FORMAT) -> Result<Self, String> {
        let err = |e: windows::core::Error| e.to_string();
        let vs = crate::gpu::recomp::dxc::compile(SHADER, "vs", "vs_6_0", &[])?;
        let ps = crate::gpu::recomp::dxc::compile(SHADER, "ps", "ps_6_0", &[])?;
        let range = D3D12_DESCRIPTOR_RANGE {
            RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_SRV, NumDescriptors: 1,
            BaseShaderRegister: 0, ..Default::default()
        };
        let params = [
            D3D12_ROOT_PARAMETER {
                ParameterType: D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, ShaderVisibility: D3D12_SHADER_VISIBILITY_VERTEX,
                Anonymous: D3D12_ROOT_PARAMETER_0 { Constants: D3D12_ROOT_CONSTANTS { ShaderRegister: 0, RegisterSpace: 0, Num32BitValues: 4 } },
            },
            D3D12_ROOT_PARAMETER {
                ParameterType: D3D12_ROOT_PARAMETER_TYPE_SRV, ShaderVisibility: D3D12_SHADER_VISIBILITY_VERTEX,
                Anonymous: D3D12_ROOT_PARAMETER_0 { Descriptor: D3D12_ROOT_DESCRIPTOR { ShaderRegister: 1, RegisterSpace: 0 } },
            },
            D3D12_ROOT_PARAMETER {
                ParameterType: D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, ShaderVisibility: D3D12_SHADER_VISIBILITY_PIXEL,
                Anonymous: D3D12_ROOT_PARAMETER_0 { DescriptorTable: D3D12_ROOT_DESCRIPTOR_TABLE { NumDescriptorRanges: 1, pDescriptorRanges: &range } },
            },
        ];
        let mut blob = None;
        D3D12SerializeRootSignature(&D3D12_ROOT_SIGNATURE_DESC {
            NumParameters: params.len() as u32, pParameters: params.as_ptr(), ..Default::default()
        }, D3D_ROOT_SIGNATURE_VERSION_1, &mut blob, None).map_err(err)?;
        let blob = blob.ok_or("overlay root signature is empty")?;
        let root: ID3D12RootSignature = dev.CreateRootSignature(0, std::slice::from_raw_parts(blob.GetBufferPointer() as *const u8, blob.GetBufferSize())).map_err(err)?;
        let mut desc = D3D12_GRAPHICS_PIPELINE_STATE_DESC {
            pRootSignature: ManuallyDrop::new(Some(root.clone())),
            VS: D3D12_SHADER_BYTECODE { pShaderBytecode: vs.as_ptr() as _, BytecodeLength: vs.len() },
            PS: D3D12_SHADER_BYTECODE { pShaderBytecode: ps.as_ptr() as _, BytecodeLength: ps.len() },
            SampleMask: u32::MAX, PrimitiveTopologyType: D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
            NumRenderTargets: 1, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            RasterizerState: D3D12_RASTERIZER_DESC { FillMode: D3D12_FILL_MODE_SOLID, CullMode: D3D12_CULL_MODE_NONE, DepthClipEnable: true.into(), ..Default::default() },
            ..Default::default()
        };
        desc.RTVFormats[0] = format;
        desc.BlendState.RenderTarget[0] = D3D12_RENDER_TARGET_BLEND_DESC {
            BlendEnable: true.into(), SrcBlend: D3D12_BLEND_SRC_ALPHA, DestBlend: D3D12_BLEND_INV_SRC_ALPHA,
            BlendOp: D3D12_BLEND_OP_ADD, SrcBlendAlpha: D3D12_BLEND_ONE, DestBlendAlpha: D3D12_BLEND_ZERO,
            BlendOpAlpha: D3D12_BLEND_OP_ADD, LogicOp: D3D12_LOGIC_OP_NOOP,
            RenderTargetWriteMask: D3D12_COLOR_WRITE_ENABLE_ALL.0 as u8, ..Default::default()
        };
        let pso = dev.CreateGraphicsPipelineState(&desc);
        ManuallyDrop::drop(&mut desc.pRootSignature);
        let pso = pso.map_err(err)?;
        let srv: ID3D12DescriptorHeap = dev.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC {
            Type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, NumDescriptors: 1,
            Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, NodeMask: 0,
        }).map_err(err)?;
        let rtv = dev.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC {
            Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NumDescriptors: 1, ..Default::default()
        }).map_err(err)?;
        let mut atlas = None;
        dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() }, D3D12_HEAP_FLAG_NONE,
            &D3D12_RESOURCE_DESC {
                Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: font::WIDTH as u64, Height: font::HEIGHT as u32,
                DepthOrArraySize: 1, MipLevels: 1, Format: DXGI_FORMAT_R8_UNORM,
                SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, ..Default::default()
            }, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut atlas).map_err(err)?;
        let atlas = atlas.ok_or("overlay font texture is empty")?;
        dev.CreateShaderResourceView(&atlas, None, srv.GetCPUDescriptorHandleForHeapStart());
        let upload = buffer(dev, 256 * font::HEIGHT as u64, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ).map_err(err)?;
        let mut ptr = std::ptr::null_mut();
        upload.Map(0, Some(&D3D12_RANGE::default()), Some(&mut ptr)).map_err(err)?;
        let pixels = font::atlas();
        for y in 0..font::HEIGHT as usize {
            std::ptr::copy_nonoverlapping(pixels.as_ptr().add(y * font::WIDTH as usize), (ptr as *mut u8).add(y * 256), font::WIDTH as usize);
        }
        upload.Unmap(0, None);
        let cells = buffer(dev, (COLUMNS * ROWS * 4) as u64, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ).map_err(err)?;
        Ok(Self { root, pso, srv, rtv, atlas, upload, cells, uploaded: false })
    }

    /// Called on XCL's copy list only; no title command-list state is changed.
    pub(super) unsafe fn draw(&mut self, dev: &ID3D12Device, list: &ID3D12GraphicsCommandList, target: &ID3D12Resource, lines: &[String]) -> bool {
        let columns = lines.iter().map(String::len).max().unwrap_or(0).min(COLUMNS);
        let rows = lines.len().min(ROWS);
        if columns == 0 || rows == 0 { return false; }
        let mut ptr = std::ptr::null_mut();
        if self.cells.Map(0, Some(&D3D12_RANGE::default()), Some(&mut ptr)).is_err() { return false; }
        let cells = std::slice::from_raw_parts_mut(ptr as *mut u32, columns * rows);
        cells.fill(b' ' as u32);
        for (y, line) in lines.iter().take(rows).enumerate() {
            for (x, ch) in line.bytes().take(columns).enumerate() { cells[y * columns + x] = ch.min(127) as u32; }
        }
        self.cells.Unmap(0, None);
        if !self.uploaded {
            let dst = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&self.atlas), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, ..Default::default() };
            let src = D3D12_TEXTURE_COPY_LOCATION {
                pResource: std::mem::transmute_copy(&self.upload), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                    Offset: 0, Footprint: D3D12_SUBRESOURCE_FOOTPRINT { Format: DXGI_FORMAT_R8_UNORM, Width: font::WIDTH as u32, Height: font::HEIGHT as u32, Depth: 1, RowPitch: 256 },
                } },
            };
            list.CopyTextureRegion(&dst, 0, 0, 0, &src, None);
            list.ResourceBarrier(&[super::present::transition(&self.atlas, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)]);
        }
        let d = target.GetDesc();
        let (w, h) = (d.Width as f32, d.Height as f32);
        let scale = (w / 1280.0).max(h / 720.0).max(1.0) * 2.0;
        let layout = [w.to_bits(), h.to_bits(), scale.to_bits(), columns as u32];
        let rtv = self.rtv.GetCPUDescriptorHandleForHeapStart();
        dev.CreateRenderTargetView(target, None, rtv);
        // Adopted devices track title RTVs with an AddRef. This is XCL's transient
        // swapchain view; retaining it there would prevent ResizeBuffers.
        super::present::note_rtv(rtv.ptr, std::ptr::null_mut());
        list.ResourceBarrier(&[super::present::transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET)]);
        list.SetPipelineState(&self.pso);
        list.SetGraphicsRootSignature(&self.root);
        list.SetDescriptorHeaps(&[Some(self.srv.clone())]);
        list.SetGraphicsRoot32BitConstants(0, 4, layout.as_ptr() as *const c_void, 0);
        list.SetGraphicsRootShaderResourceView(1, self.cells.GetGPUVirtualAddress());
        list.SetGraphicsRootDescriptorTable(2, self.srv.GetGPUDescriptorHandleForHeapStart());
        list.OMSetRenderTargets(1, Some(&rtv), false, None);
        list.RSSetViewports(&[D3D12_VIEWPORT { Width: w, Height: h, MinDepth: 0.0, MaxDepth: 1.0, ..Default::default() }]);
        list.RSSetScissorRects(&[RECT { left: 0, top: 0, right: w as i32, bottom: h as i32 }]);
        list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list.DrawInstanced(6, (columns * rows) as u32, 0, 0);
        list.ResourceBarrier(&[super::present::transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST)]);
        true
    }
}

#[cfg(test)]
#[path = "overlay_tests.rs"]
mod tests;
