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
    timer,
    timer::hal::tu102, //
};

struct Gh100;

pub(crate) fn timer_time(bar: Bar0<'_>, chipset: Chipset, time_ns: u64) -> Result<u64> {
    let sec_hi2 = u32::from(bar.read(regs::gh100::NV_PGC6_SCI_SEC_TIMER_TIME_1));
    let sec_timer_ns: u64;
    loop {
        let sec_lo = u32::from(bar.read(regs::gh100::NV_PGC6_SCI_SEC_TIMER_TIME_0));
        let sec_hi = u32::from(bar.read(regs::gh100::NV_PGC6_SCI_SEC_TIMER_TIME_1));
        if sec_hi == sec_hi2 {
            sec_timer_ns = u64::from(sec_hi) << 32 | u64::from(sec_lo);
            break;
        }
    }

    let sys_timer_offset_ns = time_ns - sec_timer_ns;

    // kfspRequiresBug3957833WAR_HAL
    if chipset != Chipset::GH100 {
        bar.write_reg(
            regs::gh100::NV_PGC6_SCI_SYS_TIMER_OFFSET_1::zeroed()
                .try_with_nsec((sys_timer_offset_ns >> 32) as u32)?,
        );
        bar.write_reg(
            regs::gh100::NV_PGC6_SCI_SYS_TIMER_OFFSET_0::zeroed()
                .try_with_nsec((time_ns & 0xffffffff) as u32)?,
        );
    }
    Ok(sys_timer_offset_ns)
}

impl TimerHal for Gh100 {
    fn init(&self, chipset: Chipset, bar: Bar0<'_>) -> Result<i64> {
        let cpu: i64 = timer::get_cpu_time_ns();
        let sys_timer_offset_ns = timer_time(bar, chipset, cpu as u64)?;
        Ok(sys_timer_offset_ns as i64)
    }

    fn timer_read(&self, bar: Bar0<'_>) -> u64 {
        tu102::timer_read(bar)
    }
}

const GH100: Gh100 = Gh100;
pub(super) const GH100_HAL: &dyn TimerHal = &GH100;
