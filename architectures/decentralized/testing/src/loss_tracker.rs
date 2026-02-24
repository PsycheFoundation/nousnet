/// Tracks per-epoch loss and asserts it doesn't regress beyond a tolerance.
pub struct LossTracker {
    current_epoch: i64,
    last_epoch_loss: f64,
    tolerance: f64,
    target_epoch: u64,
}

#[derive(Debug, PartialEq, Eq)]
pub enum LossResult {
    /// No new epoch observed, or loss was NaN: keep going
    Continue,
    /// A new epoch loss was recorded
    EpochRecorded,
    /// The target epoch has been reached.
    TargetReached,
}

impl LossTracker {
    pub fn new(target_epoch: u64, tolerance: f64) -> Self {
        Self {
            current_epoch: -1,
            last_epoch_loss: f64::MAX,
            tolerance,
            target_epoch,
        }
    }

    /// Record a loss observation. Panics if the loss exceeds the tolerance.
    pub fn record(&mut self, epoch: u64, loss: Option<f64>) -> LossResult {
        if epoch as i64 <= self.current_epoch {
            return LossResult::Continue;
        }
        self.current_epoch = epoch as i64;

        let Some(loss) = loss else {
            println!("Reached new epoch but loss was NaN");
            return LossResult::EpochRecorded;
        };

        assert!(
            loss < self.last_epoch_loss * self.tolerance,
            "Loss {loss} exceeded tolerance (last: {}, tolerance: {})",
            self.last_epoch_loss,
            self.tolerance
        );
        self.last_epoch_loss = loss;

        if epoch >= self.target_epoch {
            LossResult::TargetReached
        } else {
            LossResult::EpochRecorded
        }
    }
}
