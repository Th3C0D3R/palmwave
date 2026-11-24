//! What the GPU writes into a buffer in a GPU view of the title's CPU memory is in the title's
//! page – the pool page the queue's page-mapping method put under the view – when the title's
//! own fence after that work completes (`d3d12x::gpu_view_buffers`, through
//! `sparse::readback_after`), and nothing else of that page changes.

use windows::core::Interface;
use windows::Win32::Foundation::{CloseHandle, WAIT_OBJECT_0};
use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC};
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};

use xcl::hle::d3d12x::{gpu_view_buffers, sparse};
use xcl::hle::kernelx::mirrors;

const PAGE: usize = 0x1_0000;

/// A batch of the queue's page-mapping method (`kernelx::mirrors::apply_batches`).
#[repr(C)]
struct PageBatch {
    dest: u64,
    ranges_n: u32,
    _pad: u32,
    ranges: usize,
}

#[test]
fn gpu_writes_into_a_gpu_view_buffer_reach_the_titles_page_by_its_fence() {
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
        if windows.is_empty() {
            println!("SKIPPED: no sparse windows");
            return;
        }
        let queue: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default()).unwrap();
        let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
        let ev = CreateEventW(None, false, false, None).unwrap();
        let mut value = 0u64;
        // the title's Signal of its fence and its wait for it, as the queue's Signal method
        // makes it (`real::queue_dispatch`): after the copies back that have not landed
        let mut wait = || {
            value += 1;
            let v = value;
            let hr = sparse::title_signal(queue.as_raw(), fence.as_raw(), v, || match queue.Signal(&fence, v) {
                Ok(()) => 0,
                Err(e) => e.code().0,
            });
            assert!(hr >= 0, "Signal {hr:#x}");
            fence.SetEventOnCompletion(value, ev).unwrap();
            assert_eq!(WaitForSingleObject(ev, 5000), WAIT_OBJECT_0, "the queue did not finish");
        };

        // the title's two views (address space only), its pool, and pool page 1 under page 2
        let (cpu, gpu, size) = (0x7200_0000_0000usize, 0x7206_0000_0000usize, 0x131 * PAGE);
        mirrors::reserved(cpu, size, 0x2040_2000, 0x04);
        mirrors::reserved(gpu, size, 0x2040_2000, 0x801);
        let pool: Vec<u8> = (0..3 * PAGE).map(|i| (i % 251 + 1) as u8).collect();
        let before = pool.clone();
        let p = pool.as_ptr() as usize;
        let r = [[0u32, 1, 1]];
        let batch = PageBatch { dest: (gpu + 2 * PAGE) as u64, ranges_n: 1, _pad: 0, ranges: r.as_ptr() as usize };
        assert!(mirrors::apply_batches(1, &batch as *const PageBatch as usize, p, 3).is_some());

        // a buffer on that page that the GPU may write
        let at = gpu_view_buffers::address(gpu + 2 * PAGE, PAGE, "a test buffer").expect("a mirror");
        gpu_view_buffers::mark_written(at, PAGE, "a test view");
        let u = xcl::gpu::uma::get().unwrap();
        let k = u.window_of(at).expect("the mirror is in a window");
        let Some(&(_, raw, _)) = windows.iter().find(|w| w.0 == k) else {
            println!("SKIPPED: the mirror is not in a sparse window");
            return;
        };
        let base = u.window(k).0;
        assert_eq!(std::slice::from_raw_parts(at as *const u8, PAGE), &pool[PAGE..2 * PAGE], "the mirror holds the pool page");
        assert!(gpu_view_buffers::written_ranges().is_empty(), "nothing is copied back before video memory holds the page");

        // as before each of the title's submits: the page goes to video memory
        let dirty: Vec<usize> = (at..at + PAGE).step_by(0x1000).collect();
        let look = gpu_view_buffers::look_begins();
        sparse::before_execute(&queue, &dirty, 0x1000);
        gpu_view_buffers::look_done(look);
        wait();
        assert_eq!(gpu_view_buffers::written_ranges(), vec![(at, PAGE)]);

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
        let mut m = std::ptr::null_mut();
        src.Map(0, None, Some(&mut m)).unwrap();
        let raw = raw as *mut std::ffi::c_void;
        let window = ID3D12Resource::from_raw_borrowed(&raw).unwrap();
        let al: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
        let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &al, None).unwrap();
        list.Close().unwrap();
        // five times: the GPU writes 64 bytes of the buffer (as a shader would through a UAV),
        // the title waits for its fence and reads its page
        for i in 0..5usize {
            let off = 0x1C0 + i * 0x400;
            let want = [0xA0u8 + i as u8; 64];
            std::ptr::copy_nonoverlapping(want.as_ptr(), m as *mut u8, 64);
            al.Reset().unwrap();
            list.Reset(&al, None).unwrap();
            list.CopyBufferRegion(window, (at - base + off) as u64, &src, 0, 64);
            list.Close().unwrap();
            // (the queue's ExecuteCommandLists is XCL's: it asks for the copy back itself)
            queue.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
            wait();
            assert_eq!(&pool[PAGE + off..PAGE + off + 64], &want, "round {i}: the GPU's bytes in the title's page after its fence wait");
        }
        // nothing but those five chunks changed
        for (i, (now, was)) in pool.iter().zip(&before).enumerate() {
            let written = i >= PAGE && (0..5).any(|k| (PAGE + 0x1C0 + k * 0x400..PAGE + 0x200 + k * 0x400).contains(&i));
            assert!(written || now == was, "byte {i:#x} of the pool changed");
        }
        mirrors::released(cpu);
        let _ = CloseHandle(ev);
    }
}
