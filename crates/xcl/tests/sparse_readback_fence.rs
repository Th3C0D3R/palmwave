//! A title's queue is held by a wait that a later submit ends, so the copy back of its GPU writes
//! into a sparse window cannot be waited for in its ExecuteCommandLists (`sparse_readback_waits`):
//! a thread lands it later. The title's own fence signalled on that queue after the work must
//! then not complete before the copy has landed – a title waits for its fence and reads at once
//! (a retail title: an index of -1 read where the GPU had not "written" yet, and a crash).

use std::time::{Duration, Instant};

use windows::core::Interface;
use windows::Win32::Foundation::{CloseHandle, WAIT_OBJECT_0};
use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC};
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};

use xcl::hle::d3d12x::sparse;

/// 50 times: queue A waits for F, runs a list writing 8 bytes of a sparse window, is read back
/// and signals the title's fence; queue B then signals F. The title waits for its fence and
/// reads the bytes at once.
#[test]
fn the_titles_fence_completes_after_a_late_copy_back_has_landed() {
    unsafe {
        std::env::set_var("XCL_UMA_MB", "64");
        std::env::set_var("XCL_COHERENT_MB", "64");
        let Ok(dev) = xcl::gpu::agility::create_device(None, D3D_FEATURE_LEVEL_11_0) else {
            println!("SKIPPED: no GPU");
            return;
        };
        // the title's device: its queues and fences come with XCL's methods
        xcl::hle::d3d12x::real::adopt(dev.as_raw(), xcl::hle::d3d12x::real::Kind::Device);
        let windows = sparse::init(&dev);
        let Some(&(k, raw, _)) = windows.first() else {
            println!("SKIPPED: no sparse windows");
            return;
        };
        let base = xcl::gpu::uma::get().unwrap().window(k).0;
        let (a, _) = sparse::alloc_in(k, 0x1_0000, 0x1_0000).expect("sparse allocation");
        let qa: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default()).unwrap();
        let qb: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_COMPUTE, ..Default::default() }).unwrap();
        let f: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
        let title: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();

        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
            Width: 256,
            Height: 1,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: DXGI_FORMAT_UNKNOWN,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            ..Default::default()
        };
        let mut src: Option<ID3D12Resource> = None;
        dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_UPLOAD, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, None, &mut src).unwrap();
        let src = src.unwrap();
        let mut p = std::ptr::null_mut();
        src.Map(0, None, Some(&mut p)).unwrap();
        let raw = raw as *mut std::ffi::c_void;
        let window = ID3D12Resource::from_raw_borrowed(&raw).unwrap();
        let al: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
        let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &al, None).unwrap();
        list.Close().unwrap();
        let ev = CreateEventW(None, false, false, None).unwrap();
        sparse::before_execute(&qa, &[], 0x1000);

        for i in 1..=50u64 {
            let want = 0x1122_3344_0000_0000 | i;
            *(p as *mut u64) = want;
            if i > 1 {
                // (the allocator's last list has run: the title's fence said so)
                al.Reset().unwrap();
            }
            list.Reset(&al, None).unwrap();
            list.CopyBufferRegion(window, (a - base) as u64, &src, 0, 8);
            list.Close().unwrap();
            // queue A: held until queue B's later signal
            qa.Wait(&f, i).unwrap();
            qa.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
            let t = Instant::now();
            sparse::readback_after_execute(&qa, &[(a, 8)]);
            assert!(t.elapsed() < Duration::from_secs(2), "round {i}: the readback waited for a queue that a later submit lets go");
            // the title's Signal of its fence after that work
            let hr = sparse::title_signal(qa.as_raw(), title.as_raw(), i, || match qa.Signal(&title, i) {
                Ok(()) => 0,
                Err(e) => e.code().0,
            });
            assert!(hr >= 0, "round {i}: Signal {hr:#x}");
            assert!(title.GetCompletedValue() < i, "round {i}: queue A cannot be there yet");
            // the later submit
            qb.Signal(&f, i).unwrap();
            // the title waits for its fence and reads
            if title.GetCompletedValue() < i {
                title.SetEventOnCompletion(i, ev).unwrap();
                assert_eq!(WaitForSingleObject(ev, 5000), WAIT_OBJECT_0, "round {i}: the title's fence did not complete");
            }
            let got = *(a as *const u64);
            assert_eq!(got, want, "round {i}: the CPU's view when the title's fence completes");
        }
        let _ = CloseHandle(ev);
    }
}
