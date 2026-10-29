// SPDX-License-Identifier: GPL-2.0

use kernel::prelude::*;

use crate::{
    driver::Bar0,
    gpu::{
        Architecture,
        Chipset, //
    },
};

mod gh100;
mod tu102;

pub(crate) trait TimerHal {
    /// return system timer offset ns
    fn init(&self, chipset: Chipset, bar: Bar0<'_>) -> Result<i64>;

    /// read the timer
    #[allow(dead_code)]
    fn timer_read(&self, bar: Bar0<'_>) -> u64;
}

pub(super) fn timer_hal(chipset: Chipset) -> &'static dyn TimerHal {
    match chipset.arch() {
        Architecture::Turing | Architecture::Ampere | Architecture::Ada => tu102::TU102_HAL,
        Architecture::Hopper | Architecture::BlackwellGB10x | Architecture::BlackwellGB20x => {
            gh100::GH100_HAL
        }
    }
}
