// SPDX-License-Identifier: GPL-2.0

mod hal;

use kernel::{
    prelude::*,
    time::*, //
};

use crate::{
    driver::Bar0,
    gpu::Chipset, //
};

#[pin_data]
pub(crate) struct Timer {
    chipset: Chipset,
    sys_timer_offset_ns: i64,
}

impl Timer {
    pub(crate) fn new(bar: Bar0<'_>, chipset: Chipset) -> Result<Self> {
        Ok(Self {
            chipset,
            sys_timer_offset_ns: hal::timer_hal(chipset).init(chipset, bar)?,
        })
    }

    #[expect(dead_code)]
    pub(crate) fn sys_timer_offset_ns(&self) -> i64 {
        self.sys_timer_offset_ns
    }
}

pub(crate) fn get_cpu_time_ns() -> i64 {
    Instant::<Monotonic>::now().as_nanos()
}
