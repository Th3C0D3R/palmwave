use super::*;
use windows::core::Interface;
use windows::Win32::Graphics::Dxgi::*;

#[test]
fn f10_toggles_once_and_consumes_system_key_release() {
    use windows::Win32::UI::WindowsAndMessaging::*;
    let visible = AtomicBool::new(true);
    assert!(toggle_key(&visible, WM_SYSKEYDOWN, 0x79, 0));
    assert!(!visible.load(Ordering::Relaxed));
    assert!(toggle_key(&visible, WM_SYSKEYDOWN, 0x79, 1 << 30));
    assert!(!visible.load(Ordering::Relaxed));
    assert!(toggle_key(&visible, WM_SYSKEYUP, 0x79, 1 << 30));
    assert!(!visible.load(Ordering::Relaxed));
    assert!(toggle_key(&visible, WM_KEYDOWN, 0x79, 0));
    assert!(visible.load(Ordering::Relaxed));
    assert!(!toggle_key(&visible, WM_SYSKEYDOWN, 0x78, 0));
    assert!(!toggle_key(&visible, WM_TIMER, 0x79, 0));
}

#[test]
fn warp_draws_font_and_blends_panel_without_changing_surroundings() {
    if crate::gpu::recomp::dxc::location().is_err() {
        crate::gpu::recomp::runner::skip_test("no DXC");
        return;
    }
    unsafe {
        let factory: IDXGIFactory4 = CreateDXGIFactory2(DXGI_CREATE_FACTORY_FLAGS(0)).unwrap();
        let adapter: IDXGIAdapter = factory.EnumWarpAdapter().unwrap();
        let dev = match crate::gpu::agility::create_device(&adapter, D3D_FEATURE_LEVEL_11_0) {
            Ok(dev) => dev,
            Err(_) => {
                crate::gpu::recomp::runner::skip_test("no GPU");
                return;
            }
        };
        let queue: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC {
            Type: D3D12_COMMAND_LIST_TYPE_DIRECT, ..Default::default()
        }).unwrap();
        let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
        let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
        let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
        list.Close().unwrap();
        let mut overlay = Overlay::new(&dev, DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
        let mut timing = super::super::overlay_timing::Timing::new(&dev, &queue).expect("direct queues support timestamps");
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: 128, Height: 64,
            DepthOrArraySize: 1, MipLevels: 1, Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, ..Default::default()
        };
        let mut target: Option<ID3D12Resource> = None;
        dev.CreateCommittedResource(
            &D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() },
            D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, None, &mut target,
        ).unwrap();
        let target = target.unwrap();
        let heap: ID3D12DescriptorHeap = dev.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC {
            Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NumDescriptors: 1, ..Default::default()
        }).unwrap();
        let rtv = heap.GetCPUDescriptorHandleForHeapStart();
        dev.CreateRenderTargetView(&target, None, rtv);
        let mut footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default();
        let mut bytes = 0;
        dev.GetCopyableFootprints(&desc, 0, 1, 0, Some(&mut footprint), None, None, Some(&mut bytes));
        let readback = buffer(&dev, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST).unwrap();
        let atlas = font::atlas();
        for (frame, lines) in [vec!["A ".to_string()], vec![" Z".to_string(), "A".to_string()]].iter().enumerate() {
            alloc.Reset().unwrap();
            list.Reset(&alloc, None).unwrap();
            timing.stamp(&list, 0);
            list.ClearRenderTargetView(rtv, &[0.2, 0.4, 0.6, 1.0], None);
            timing.stamp(&list, 1);
            list.ResourceBarrier(&[super::super::present::transition(&target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST)]);
            timing.stamp(&list, 2);
            assert!(overlay.draw(&dev, &list, &target, lines));
            timing.stamp(&list, 3);
            timing.resolve(&list);
            list.ResourceBarrier(&[super::super::present::transition(&target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
            let to = D3D12_TEXTURE_COPY_LOCATION {
                pResource: std::mem::transmute_copy(&readback), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: footprint },
            };
            let from = D3D12_TEXTURE_COPY_LOCATION {
                pResource: std::mem::transmute_copy(&target), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 },
            };
            list.CopyTextureRegion(&to, 0, 0, 0, &from, None);
            list.ResourceBarrier(&[super::super::present::transition(&target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)]);
            list.Close().unwrap();
            queue.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
            queue.Signal(&fence, frame as u64 + 1).unwrap();
            overlay.submitted();
            timing.submitted();
            crate::gpu::fence::wait(&fence, frame as u64 + 1, &dev, None, 10_000).unwrap();
            timing.completed();
            assert!(timing.copy_ms.is_some_and(|ms| ms.is_finite() && ms >= 0.0));
            assert!(timing.overlay_ms.is_some_and(|ms| ms.is_finite() && ms >= 0.0));
            let mut ptr = std::ptr::null_mut();
            readback.Map(0, Some(&D3D12_RANGE { Begin: 0, End: bytes as usize }), Some(&mut ptr)).unwrap();
            let pixels = std::slice::from_raw_parts(ptr as *const u8, bytes as usize).to_vec();
            readback.Unmap(0, Some(&D3D12_RANGE::default()));
            let (mut ink, mut panel, mut outside) = (0, 0, 0);
            for y in 0..64usize {
                for x in 0..128usize {
                    let expected = if (12..44).contains(&x) && (12..12 + lines.len() * 16).contains(&y) {
                        let (cx, cy) = ((x - 12) / 16, (y - 12) / 16);
                        let glyph = lines[cy].as_bytes().get(cx).copied().unwrap_or(b' ') as usize;
                        let (gx, gy) = (((x - 12) % 16) / 2, ((y - 12) % 16) / 2);
                        if atlas[(glyph / 16 * 8 + gy) * font::WIDTH as usize + glyph % 16 * 8 + gx] > 127 {
                            ink += 1;
                            [191u8, 255, 217, 255]
                        } else {
                            panel += 1;
                            [10, 20, 31, 204]
                        }
                    } else {
                        outside += 1;
                        [51, 102, 153, 255]
                    };
                    let offset = footprint.Offset as usize + y * footprint.Footprint.RowPitch as usize + x * 4;
                    let actual = &pixels[offset..offset + 4];
                    assert!(actual.iter().zip(expected).all(|(&a, b)| a.abs_diff(b) <= 1),
                        "frame {frame}, pixel ({x}, {y}): {actual:?}, expected {expected:?}");
                }
            }
            assert!(ink > 0 && panel > 0 && outside > 0, "each frame must check glyphs, panel blending and untouched pixels");
        }
    }
}
