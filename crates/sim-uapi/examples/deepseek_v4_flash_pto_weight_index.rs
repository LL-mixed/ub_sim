#[path = "../src/deepseek_v4_flash_pto_weights.rs"]
mod deepseek_v4_flash_pto_weights;

use std::ffi::OsString;
use std::fs;
use std::path::PathBuf;

fn parse_args(args: impl IntoIterator<Item = OsString>) -> Result<(PathBuf, PathBuf), String> {
    let mut args = args.into_iter();
    let model = args
        .next()
        .map(PathBuf::from)
        .ok_or("usage: deepseek_v4_flash_pto_weight_index MODEL_GGUF OUTPUT_INDEX")?;
    let output = args
        .next()
        .map(PathBuf::from)
        .ok_or("usage: deepseek_v4_flash_pto_weight_index MODEL_GGUF OUTPUT_INDEX")?;
    if args.next().is_some() {
        return Err("usage: deepseek_v4_flash_pto_weight_index MODEL_GGUF OUTPUT_INDEX".into());
    }
    Ok((model, output))
}

fn main() {
    let result = (|| {
        let (model, output) = parse_args(std::env::args_os().skip(1))?;
        let index = deepseek_v4_flash_pto_weights::private_weight_index(&model)?;
        fs::write(&output, &index).map_err(|error| {
            format!("pto_weight_index_write_failed:{}:{error}", output.display())
        })?;
        println!(
            "pto_weight_index=pass bytes={} output={}",
            index.len(),
            output.display()
        );
        Ok::<(), String>(())
    })();
    if let Err(error) = result {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requires_exactly_model_and_output_paths() {
        assert!(parse_args([]).is_err());
        assert!(parse_args([OsString::from("model.gguf")]).is_err());
        assert!(parse_args([
            OsString::from("model.gguf"),
            OsString::from("index.bin"),
            OsString::from("extra"),
        ])
        .is_err());
        assert_eq!(
            parse_args([OsString::from("model.gguf"), OsString::from("index.bin")]).unwrap(),
            (PathBuf::from("model.gguf"), PathBuf::from("index.bin"))
        );
    }
}
