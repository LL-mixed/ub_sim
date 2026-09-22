use sim_models::deepseek_v4_flash::DEEPSEEK_V4_FLASH_PROFILE;
use sim_models::deepseek_v4_flash_gguf::GgufCatalog;
use std::env;
use std::fs;
use std::path::PathBuf;

fn usage() -> String {
    "usage: deepseek_v4_flash_pto_token_table --model FILE --output FILE".to_string()
}

fn parse_args() -> Result<(PathBuf, PathBuf), String> {
    let mut model = None;
    let mut output = None;
    let mut args = env::args().skip(1);

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--model" => {
                model = Some(PathBuf::from(args.next().ok_or_else(usage)?));
            }
            "--output" => {
                output = Some(PathBuf::from(args.next().ok_or_else(usage)?));
            }
            _ => return Err(usage()),
        }
    }
    Ok((model.ok_or_else(usage)?, output.ok_or_else(usage)?))
}

fn run() -> Result<(), String> {
    let (model, output) = parse_args()?;
    let catalog = GgufCatalog::open(&model)?;
    let table = catalog.pto_token_table()?;
    let vocab = u64::from_le_bytes(
        table[16..24]
            .try_into()
            .map_err(|_| "deepseek_pto_token_table_header_invalid")?,
    );

    if vocab != DEEPSEEK_V4_FLASH_PROFILE.vocab_size {
        return Err(format!(
            "deepseek_pto_token_table_vocab_mismatch:actual={vocab}:expected={}",
            DEEPSEEK_V4_FLASH_PROFILE.vocab_size
        ));
    }
    fs::write(&output, &table).map_err(|err| {
        format!(
            "deepseek_pto_token_table_write_failed:{}:{err}",
            output.display()
        )
    })?;
    println!(
        "deepseek_pto_token_table=pass vocab={vocab} bytes={} output={}",
        table.len(),
        output.display()
    );
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(2);
    }
}
