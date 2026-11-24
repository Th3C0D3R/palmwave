use windows_sys::Win32::System::Memory::{MEM_DECOMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_NOACCESS, PAGE_READWRITE};
use xcl::hle::kernelx::{gpu_view_map_gen, in_graphics_reservation, mirrors, VirtualAlloc, VirtualFree};

// In its own process so no other test can allocate the address just released.
#[test]
fn sparse_64k_views_pair_and_forget_reused_addresses() {
    let Some(uma) = xcl::gpu::uma::get().filter(|u| u.has_sparse()) else {
        xcl::gpu::recomp::runner::skip_test("no sparse UMA address space");
        return;
    };
    let size = 3 * 65536;
    let reserve = |size, protection| {
        let p = unsafe { VirtualAlloc(std::ptr::null(), size, MEM_RESERVE | 0x2040_0000, protection) };
        assert!(!p.is_null(), "failed to reserve {size:#x} bytes with protection {protection:#x}");
        p
    };
    let gpu_protection = PAGE_NOACCESS | 0x800; // PAGE_GRAPHICS_NOACCESS
    let cpu = reserve(size, PAGE_READWRITE);
    let gpu = reserve(size, gpu_protection);
    assert!(uma.is_sparse(cpu as usize) && uma.is_sparse(gpu as usize));
    let sample = gpu as usize + 65536;
    let backing = cpu as usize + 65536;
    assert_eq!(mirrors::source(sample), Some(backing));

    unsafe {
        let paired = gpu_view_map_gen();
        assert_eq!(VirtualFree(gpu, size, MEM_RELEASE), 0);
        assert_eq!(VirtualFree(gpu, size, MEM_DECOMMIT), 1);
        assert_eq!(gpu_view_map_gen(), paired);
        assert_eq!(mirrors::source(sample), Some(backing));

        assert_eq!(VirtualFree(gpu, 0, MEM_RELEASE), 1);
        assert_eq!(mirrors::source(sample), None);
        assert_eq!(gpu_view_map_gen(), paired + 1);
        let replacement = reserve(size, PAGE_READWRITE);
        assert_eq!(replacement, gpu);
        assert_eq!(mirrors::source(sample), None);
        assert_eq!(VirtualFree(replacement, 0, MEM_RELEASE), 1);
        assert_eq!(gpu_view_map_gen(), paired + 1);

        let gpu_again = reserve(size, gpu_protection);
        assert_eq!(gpu_again, gpu);
        assert_eq!(mirrors::source(sample), Some(backing));
        let paired = gpu_view_map_gen();
        assert_eq!(VirtualFree(cpu, 0, MEM_RELEASE), 1);
        assert_eq!(mirrors::source(sample), None);
        assert_eq!(gpu_view_map_gen(), paired + 1);
        assert_eq!(VirtualFree(gpu_again, 0, MEM_RELEASE), 1);
        assert_eq!(gpu_view_map_gen(), paired + 1);

        // Only the first of two reservations this large fits in the sparse windows.
        let sparse_bytes: usize = uma.watch_regions().into_iter()
            .filter(|&(base, _)| uma.is_sparse(base)).map(|(_, len)| len).sum();
        let size = sparse_bytes / 2 + 65536;
        let cpu = reserve(size, PAGE_READWRITE);
        let gpu = reserve(size, gpu_protection);
        assert!(uma.is_sparse(cpu as usize));
        assert!(!uma.contains(gpu as usize) && in_graphics_reservation(gpu as usize));
        assert_eq!(mirrors::source(gpu as usize + 65536), Some(cpu as usize + 65536));
        let paired = gpu_view_map_gen();
        assert_eq!(VirtualFree(gpu, 0, MEM_RELEASE), 1);
        assert_eq!(mirrors::source(gpu as usize + 65536), None);
        assert_eq!(gpu_view_map_gen(), paired + 1);
        assert_eq!(VirtualFree(cpu, 0, MEM_RELEASE), 1);
    }
}
