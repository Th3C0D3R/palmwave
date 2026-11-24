//! Four timestamps on XCL's copy list, read only after its existing completion fence.

use windows::Win32::Graphics::Direct3D12::*;

pub(super) struct Timing {
    heap: ID3D12QueryHeap,
    readback: ID3D12Resource,
    hz: u64,
    pending: bool,
    pub copy_ms: Option<f64>,
    pub overlay_ms: Option<f64>,
}

impl Timing {
    pub(super) unsafe fn new(dev: &ID3D12Device, queue: &ID3D12CommandQueue) -> Option<Self> {
        let hz = queue.GetTimestampFrequency().ok().filter(|&v| v > 0)?;
        let mut heap = None;
        dev.CreateQueryHeap(&D3D12_QUERY_HEAP_DESC {
            Type: D3D12_QUERY_HEAP_TYPE_TIMESTAMP, Count: 4, NodeMask: 0,
        }, &mut heap).ok()?;
        let readback = super::overlay::buffer(dev, 32, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST).ok()?;
        Some(Self { heap: heap?, readback, hz, pending: false, copy_ms: None, overlay_ms: None })
    }

    pub(super) unsafe fn completed(&mut self) {
        if !self.pending { return; }
        self.pending = false;
        self.copy_ms = None;
        self.overlay_ms = None;
        let mut ptr = std::ptr::null_mut();
        if self.readback.Map(0, Some(&D3D12_RANGE { Begin: 0, End: 32 }), Some(&mut ptr)).is_ok() {
            let ticks = std::slice::from_raw_parts(ptr as *const u64, 4);
            self.copy_ms = elapsed_ms(ticks[0], ticks[1], self.hz);
            self.overlay_ms = elapsed_ms(ticks[2], ticks[3], self.hz);
            self.readback.Unmap(0, Some(&D3D12_RANGE::default()));
        }
    }

    pub(super) unsafe fn stamp(&self, list: &ID3D12GraphicsCommandList, index: u32) {
        list.EndQuery(&self.heap, D3D12_QUERY_TYPE_TIMESTAMP, index);
    }

    pub(super) unsafe fn resolve(&mut self, list: &ID3D12GraphicsCommandList) {
        list.ResolveQueryData(&self.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, &self.readback, 0);
    }

    pub(super) fn submitted(&mut self) { self.pending = true; }
}

fn elapsed_ms(start: u64, end: u64, hz: u64) -> Option<f64> {
    if hz == 0 { return None; }
    Some(end.checked_sub(start)? as f64 * 1000.0 / hz as f64)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timestamps_use_queue_frequency_and_reject_invalid_ranges() {
        assert_eq!(elapsed_ms(100, 350, 1_000_000), Some(0.25));
        assert_eq!(elapsed_ms(350, 100, 1_000_000), None);
        assert_eq!(elapsed_ms(100, 350, 0), None);
        assert_eq!(elapsed_ms(100, 100, 1_000_000), Some(0.0));
    }
}
