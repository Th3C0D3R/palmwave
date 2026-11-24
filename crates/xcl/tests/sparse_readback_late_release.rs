//! The copy back of a title queue's GPU writes into a sparse window
//! (`sparse::readback_after_execute`) is submitted on that queue, through the title queue's own
//! ExecuteCommandLists. With a last `Release` held back (`late_release`, a queue still busy),
//! that submit looks at every queue XCL knows of, the sparse windows' one too – under the sparse
//! lock the thread waited for itself, and every other thread after it (a retail title: no frame
//! after its 80th).

use std::time::Duration;

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC};

use xcl::hle::d3d12x::sparse;

#[test]
fn a_readback_with_a_last_release_held_back_does_not_lock_itself() {
    unsafe {
        std::env::set_var("XCL_UMA_MB", "64");
        std::env::set_var("XCL_COHERENT_MB", "64");
        let Ok(dev) = xcl::gpu::agility::create_device(None, D3D_FEATURE_LEVEL_11_0) else {
            println!("SKIPPED: no GPU");
            return;
        };
        // the title's device: its queues, lists and resources come with XCL's methods
        xcl::hle::d3d12x::real::adopt(dev.as_raw(), xcl::hle::d3d12x::real::Kind::Device);
        let windows = sparse::init(&dev);
        let Some(&(k, raw, _)) = windows.first() else {
            println!("SKIPPED: no sparse windows");
            return;
        };
        let base = xcl::gpu::uma::get().unwrap().window(k).0;
        let (a, _) = sparse::alloc_in(k, 0x1_0000, 0x1_0000).expect("sparse allocation");
        let queue: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default()).unwrap();
        let busy: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default()).unwrap();
        let gate: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();

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
        let buffer = |heap: D3D12_HEAP_TYPE, state: D3D12_RESOURCE_STATES| {
            let mut res: Option<ID3D12Resource> = None;
            dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: heap, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &desc, state, None, &mut res).unwrap();
            res.unwrap()
        };
        let src = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        let mut p = std::ptr::null_mut();
        src.Map(0, None, Some(&mut p)).unwrap();
        *(p as *mut u64) = 0x1122_3344_5566_7788;
        let raw = raw as *mut std::ffi::c_void;
        let window = ID3D12Resource::from_raw_borrowed(&raw).unwrap();

        let list = |ty: D3D12_COMMAND_LIST_TYPE| {
            let al: ID3D12CommandAllocator = dev.CreateCommandAllocator(ty).unwrap();
            let l: ID3D12GraphicsCommandList = dev.CreateCommandList(0, ty, &al, None).unwrap();
            (al, l)
        };
        // a queue of the title's that stands still: it has run a list (XCL knows it) and waits
        // for a fence nobody has signalled yet
        let (_bal, bl) = list(D3D12_COMMAND_LIST_TYPE_DIRECT);
        bl.Close().unwrap();
        sparse::before_execute(&busy, &[], 0x1000);
        busy.ExecuteCommandLists(&[Some(bl.cast().unwrap())]);
        busy.Wait(&gate, 1).unwrap();
        // the title's last Release of a resource: held back until that queue is past it
        drop(buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON));

        // the title's submit: 8 bytes into the window, read back
        let (_al, l) = list(D3D12_COMMAND_LIST_TYPE_DIRECT);
        l.CopyBufferRegion(window, (a - base) as u64, &src, 0, 8);
        l.Close().unwrap();
        sparse::before_execute(&queue, &[], 0x1000);
        let (tx, rx) = std::sync::mpsc::channel();
        let (queue2, l) = (queue.clone(), l.cast::<ID3D12CommandList>().unwrap());
        std::thread::spawn(move || {
            queue2.ExecuteCommandLists(&[Some(l)]);
            sparse::readback_after_execute(&queue2, &[(a, 8)]);
            let _ = tx.send(());
        });
        let returned = rx.recv_timeout(Duration::from_secs(5)).is_ok();
        gate.Signal(1).unwrap();
        if !returned {
            // (no unwinding: a release here would wait for the sparse lock too)
            eprintln!("FAILED: the readback did not return in 5 s: its submit on the title's queue waits for the sparse lock its own thread holds");
            std::process::exit(101);
        }
        assert_eq!(*(a as *const u64), 0x1122_3344_5566_7788, "the GPU's write is in the CPU's view when the readback returns");
    }
}
