pub const DATA_FILE_EXTENSIONS: [&str; 3] = ["npy", "bin", "ds"];
pub const PARQUET_EXTENSION: &str = "parquet";

/// File extensions downloaded when fetching a model repository. Besides the
/// weights, these cover the config/tokenizer files that must be re-uploaded
/// with every checkpoint so the checkpoint repo stays a complete, loadable
/// model repository on its own.
pub const MODEL_FILE_EXTENSIONS: [&str; 6] = [
    ".safetensors", ".json", ".py", ".txt", ".model", ".jinja",
];
