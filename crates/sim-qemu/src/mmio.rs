use sim_core::SimError;

use crate::device::MmioDevice;

#[derive(Debug)]
pub struct QemuMmioHandler<D> {
    device: D,
}

impl<D> QemuMmioHandler<D>
where
    D: MmioDevice,
{
    pub fn new(device: D) -> Self {
        Self { device }
    }

    pub fn read(&mut self, offset: u64) -> Result<u64, SimError> {
        self.device.mmio_read(offset)
    }

    pub fn write(&mut self, offset: u64, value: u64) -> Result<(), SimError> {
        self.device.mmio_write(offset, value)
    }

    pub fn device(&self) -> &D {
        &self.device
    }

    pub fn device_mut(&mut self) -> &mut D {
        &mut self.device
    }

    pub fn into_inner(self) -> D {
        self.device
    }
}
