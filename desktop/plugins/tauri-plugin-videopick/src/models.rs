use serde::{Deserialize, Serialize};

/// 고른 영상. path 가 비어 있으면 사람이 취소한 것이다.
#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Picked {
  pub path: String,
  #[serde(default)]
  pub name: String,
  #[serde(default)]
  pub size: u64,
  /// 찍은 시각 (1970년부터의 밀리초). 0 이면 못 읽은 것이다.
  #[serde(default)]
  pub shot_at: f64,
}

#[derive(Debug, Default, Deserialize, Serialize)]
pub struct PickRequest {}
