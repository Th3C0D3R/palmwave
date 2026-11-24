//! A title's compute queue runs a list that starts with `Wait64BitValueX` on a label the direct
//! queue writes (`Write64BitValueBottomOfPipeX`) in a list the title submits later, from the same
//! thread. The wait is bridged to a GPU `queue.Wait` (`list_x::try_gpu_wait`), so the list cannot
//! run before that later submit. When it also writes a sparse window, the copy back to the CPU's
//! view (`sparse::readback_after_execute`) must not wait for it on the CPU: the title's thread
//! would never get to the submit that lets it run (a retail title: one submit per watchdog
//! release).

use std::sync::atomic::{AtomicU64, Ordering};

use windows::core::Interface;
use windows::Win32::Foundation::{CloseHandle, WAIT_OBJECT_0};
use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC};
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};

use xcl::hle::d3d12x::{list_x, sparse};

const LABEL: u64 = 0x513;

/// The title's thread: the compute queue gets a list that waits for the label and writes 8 bytes
/// of a sparse window, and is read back; then the direct queue gets the list that writes the
/// label. The compute queue finishes long before the watchdog would release its wait.
#[test]
fn a_readback_behind_a_wait64_for_a_label_submitted_later_does_not_hang() {
    unsafe {
        std::env::set_var("XCL_UMA_MB", "64");
        std::env::set_var("XCL_COHERENT_MB", "64");
        // not the watchdog's release: the later submit has to let the queue go
        std::env::set_var("XCL_GPU_WAIT_WATCHDOG_MS", "60000");
        let Ok(dev) = xcl::gpu::agility::create_device(None, D3D_FEATURE_LEVEL_11_0) else {
            println!("SKIPPED: no GPU");
            return;
        };
        // the title's device: its queues, lists and fences come with XCL's methods
        xcl::hle::d3d12x::real::adopt(dev.as_raw(), xcl::hle::d3d12x::real::Kind::Device);
        let windows = sparse::init(&dev);
        let Some(&(k, raw, _)) = windows.first() else {
            println!("SKIPPED: no sparse windows");
            return;
        };
        let base = xcl::gpu::uma::get().unwrap().window(k).0;
        let (a, _) = sparse::alloc_in(k, 0x1_0000, 0x1_0000).expect("sparse allocation");
        let compute: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_COMPUTE, ..Default::default() }).unwrap();
        let direct: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default()).unwrap();
        let done: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();

        // the label: memory of the title's own
        let label: &'static AtomicU64 = Box::leak(Box::new(AtomicU64::new(0)));
        let label_va = label as *const AtomicU64 as u64;

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
        *(p as *mut u64) = 0x1122_3344_5566_7788;
        let raw = raw as *mut std::ffi::c_void;
        let window = ID3D12Resource::from_raw_borrowed(&raw).unwrap();

        // the direct queue's list, recorded first and submitted last: it writes the label
        let da: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
        let dl: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &da, None).unwrap();
        list_x::on_create(dl.as_raw(), da.as_raw() as usize, 0, D3D12_COMMAND_LIST_TYPE_DIRECT);
        list_x::write64_bottom_x(dl.as_raw(), label_va, LABEL, 0, 0);
        dl.Close().unwrap();

        // the compute queue's list: the wait for the label, then 8 bytes into the window
        let ca: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE).unwrap();
        let cl: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, &ca, None).unwrap();
        list_x::on_create(cl.as_raw(), ca.as_raw() as usize, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        list_x::wait64_x(cl.as_raw(), label_va, D3D12_COMPARISON_FUNC_GREATER_EQUAL.0 as u32, LABEL, 0, 0);
        cl.CopyBufferRegion(window, (a - base) as u64, &src, 0, 8);
        cl.Close().unwrap();
        sparse::before_execute(&compute, &[], 0x1000);

        // the title's thread
        let (compute2, direct2, done2) = (compute.clone(), direct.clone(), done.clone());
        let (cl, dl) = (cl.cast::<ID3D12CommandList>().unwrap(), dl.cast::<ID3D12CommandList>().unwrap());
        let title = std::thread::spawn(move || {
            compute2.ExecuteCommandLists(&[Some(cl)]);
            sparse::readback_after_execute(&compute2, &[(a, 8)]);
            compute2.Signal(&done2, 1).unwrap();
            sparse::before_execute(&direct2, &[], 0x1000);
            direct2.ExecuteCommandLists(&[Some(dl)]);
        });

        let ev = CreateEventW(None, false, false, None).unwrap();
        done.SetEventOnCompletion(1, ev).unwrap();
        let finished = WaitForSingleObject(ev, 3000) == WAIT_OBJECT_0;
        title.join().unwrap();
        if !finished {
            // (the direct queue's list is submitted by now: the compute queue gets there)
            let _ = WaitForSingleObject(ev, 10_000);
        }
        assert!(finished, "the compute queue did not finish in 3 s: the readback waits on the CPU for it, and it waits for the label of the direct queue's later submit");
        // the GPU's write reaches the CPU's view, and the label the title's memory
        let landed = |read: &dyn Fn() -> u64, want: u64| {
            let mut got = read();
            for _ in 0..200 {
                if got == want {
                    break;
                }
                let _ = WaitForSingleObject(ev, 10);
                got = read();
            }
            got
        };
        assert_eq!(landed(&|| *(a as *const u64), 0x1122_3344_5566_7788), 0x1122_3344_5566_7788, "the GPU's write did not reach the CPU's view");
        assert_eq!(landed(&|| label.load(Ordering::Acquire), LABEL), LABEL, "the label was not written");
        let _ = CloseHandle(ev);
    }
}
