// SPDX-License-Identifier: GPL-2.0

use super::TimerHal;

use kernel::{
    io::Io,
    prelude::*, //
};

use crate::{
    driver::Bar0,
    gpu::Chipset,
    regs,
    timer, //
};
struct Tu102;

pub(crate) fn timer_read(bar: Bar0<'_>) -> u64 {
    let hi2: u32 = u32::from(bar.read(regs::NV_PTIMER_TIME_1));
    loop {
        let lo = u32::from(bar.read(regs::NV_PTIMER_TIME_0));
        let hi = u32::from(bar.read(regs::NV_PTIMER_TIME_1));
        if hi == hi2 {
            return u64::from(hi) << 32 | u64::from(lo);
        }
    }
}

pub(crate) fn timer_time(bar: Bar0<'_>, time_ns: u64) -> Result {
    if bar.read(regs::NV_PTIMER_TIME_PRIV_LEVEL_MASK).level0() {
        bar.write_reg(regs::NV_PTIMER_TIME_1::zeroed().try_with_nsec((time_ns >> 32) as u32)?);
        bar.write_reg(
            regs::NV_PTIMER_TIME_0::zeroed().try_with_nsec((time_ns & 0xffffffff) as u32)?,
        );
        Ok(())
    } else {
        Err(EINVAL)
    }
}

impl TimerHal for Tu102 {
    fn init(&self, _chipset: Chipset, bar: Bar0<'_>) -> Result<i64> {
        let cpu = timer::get_cpu_time_ns();

        timer_time(bar, cpu as u64)?;
        Ok(0_i64)
    }

    fn timer_read(&self, bar: Bar0<'_>) -> u64 {
        timer_read(bar)
    }
}

const TU102: Tu102 = Tu102;
pub(super) const TU102_HAL: &dyn TimerHal = &TU102;
