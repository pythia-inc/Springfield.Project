// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// ** IMPORTANT **
//
// This file is required in order to include the ones generated by
// 'glean-parser' from the SDK registry files.

pub mod error {
    use crate::metrics::CounterMetric;
    use crate::{CommonMetricData, Lifetime};
    use once_cell::sync::Lazy;

    #[allow(non_upper_case_globals)]
    pub static preinit_tasks_overflow: Lazy<CounterMetric> = Lazy::new(|| {
        CounterMetric::new(CommonMetricData {
            category: "glean.error".into(),
            name: "preinit_tasks_overflow".into(),
            send_in_pings: vec!["metrics".into()],
            lifetime: Lifetime::Ping,
            disabled: false,
            ..Default::default()
        })
    });
}
