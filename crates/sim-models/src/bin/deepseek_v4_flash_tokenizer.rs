use std::path::PathBuf;

use sim_models::deepseek_v4_flash_gguf::{DeepseekChatThinkMode, GgufCatalog};

#[derive(Clone, Debug, PartialEq, Eq)]
struct Args {
    model: PathBuf,
    prompt: String,
    system: String,
    plain: bool,
    token_ids_only: bool,
    think_mode: DeepseekChatThinkMode,
}

fn parse_args<I, S>(args: I) -> Result<Args, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let mut model = None;
    let mut prompt = None;
    let mut system = String::new();
    let mut plain = false;
    let mut token_ids_only = false;
    let mut think_mode = DeepseekChatThinkMode::NoThink;
    let mut pending = args.into_iter().map(Into::into).peekable();

    while let Some(argument) = pending.next() {
        if argument == "--model" {
            model = Some(PathBuf::from(
                pending
                    .next()
                    .ok_or_else(|| "--model requires a value".to_string())?,
            ));
        } else if let Some(value) = argument.strip_prefix("--model=") {
            model = Some(PathBuf::from(value));
        } else if argument == "--prompt" {
            prompt = Some(
                pending
                    .next()
                    .ok_or_else(|| "--prompt requires a value".to_string())?,
            );
        } else if let Some(value) = argument.strip_prefix("--prompt=") {
            prompt = Some(value.to_string());
        } else if argument == "--system" {
            system = pending
                .next()
                .ok_or_else(|| "--system requires a value".to_string())?;
        } else if let Some(value) = argument.strip_prefix("--system=") {
            system = value.to_string();
        } else if argument == "--plain" {
            plain = true;
        } else if argument == "--token-ids-only" {
            token_ids_only = true;
        } else if argument == "--think" {
            think_mode = DeepseekChatThinkMode::Think;
        } else if argument == "--no-think" {
            think_mode = DeepseekChatThinkMode::NoThink;
        } else {
            return Err(format!("unknown argument: {argument}"));
        }
    }

    Ok(Args {
        model: model.ok_or_else(|| "--model is required".to_string())?,
        prompt: prompt.ok_or_else(|| "--prompt is required".to_string())?,
        system,
        plain,
        token_ids_only,
        think_mode,
    })
}

fn run(args: Args) -> Result<(), String> {
    let catalog = GgufCatalog::open(&args.model)?;
    let tokenizer = catalog.tokenizer()?;
    let token_ids = if args.plain {
        tokenizer.tokenize_text(&args.prompt)?
    } else {
        tokenizer.tokenize_chat_prompt(&args.system, &args.prompt, args.think_mode)?
    };
    if args.token_ids_only {
        println!(
            "{}",
            token_ids
                .iter()
                .map(usize::to_string)
                .collect::<Vec<_>>()
                .join(",")
        );
    } else {
        println!(
            "{}",
            serde_json::json!({
                "mode": if args.plain { "plain" } else { "chat" },
                "model": args.model,
                "prompt": args.prompt,
                "system": args.system,
                "think": args.think_mode == DeepseekChatThinkMode::Think,
                "token_count": token_ids.len(),
                "token_ids": token_ids,
            })
        );
    }
    Ok(())
}

fn main() {
    let args = parse_args(std::env::args().skip(1)).and_then(run);
    if let Err(error) = args {
        eprintln!("deepseek_v4_flash_tokenizer: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_chat_prompt_with_explicit_think_mode() {
        let args = parse_args([
            "--model",
            "/tmp/model.gguf",
            "--prompt=hello",
            "--system",
            "brief",
            "--think",
            "--token-ids-only",
        ])
        .expect("parse tokenizer arguments");

        assert_eq!(args.model, PathBuf::from("/tmp/model.gguf"));
        assert_eq!(args.prompt, "hello");
        assert_eq!(args.system, "brief");
        assert!(!args.plain);
        assert!(args.token_ids_only);
        assert_eq!(args.think_mode, DeepseekChatThinkMode::Think);
    }

    #[test]
    fn requires_model_and_prompt() {
        assert_eq!(
            parse_args(["--prompt", "hello"]),
            Err("--model is required".to_string())
        );
        assert_eq!(
            parse_args(["--model", "/tmp/model.gguf"]),
            Err("--prompt is required".to_string())
        );
    }
}
