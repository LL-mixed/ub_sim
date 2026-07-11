use sim_config::ScenarioConfig;
use sim_core::{HierarchyCoord, LogicalSystemId, TaskKey};
use sim_models::deepseek_v4_flash::{
    DEEPSEEK_V4_FLASH_PROFILE, DEEPSEEK_V4_FLASH_RMS_EPS, DEEPSEEK_V4_FLASH_SWIGLU_CLAMP,
};
use sim_models::deepseek_v4_flash_gguf::{decode_f32_tensor, GgufCatalog};
use sim_topology::SimTopology;
use sim_uapi::{
    execute_deepseek_ffn_through_simpler, execute_deepseek_grouped_q8_projection_through_simpler,
    execute_deepseek_moe_through_simpler, execute_deepseek_q2_k_expert_projection_through_simpler,
    execute_deepseek_q8_projection_through_simpler, execute_deepseek_routed_expert_through_simpler,
    execute_deepseek_router_through_simpler, execute_deepseek_shared_expert_through_simpler,
    DeepseekV4FlashFfnWeights, DeepseekV4FlashMoeWeights,
};
use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;

#[derive(Debug, PartialEq, Eq)]
struct ProjectQ8Args {
    scenario: PathBuf,
    model: PathBuf,
    tensor: String,
    input: PathBuf,
    output: PathBuf,
    manifest: PathBuf,
    groups: usize,
}

#[derive(Debug, PartialEq, Eq)]
struct SharedExpertArgs {
    scenario: PathBuf,
    model: PathBuf,
    layer: u64,
    input: PathBuf,
    output: PathBuf,
    artifact_dir: PathBuf,
}

#[derive(Debug, PartialEq, Eq)]
struct RouterArgs {
    scenario: PathBuf,
    model: PathBuf,
    layer: u64,
    token: Option<usize>,
    input: PathBuf,
    output: PathBuf,
    manifest: PathBuf,
}

#[derive(Debug, PartialEq, Eq)]
struct RoutedDownArgs {
    scenario: PathBuf,
    model: PathBuf,
    layer: u64,
    expert: usize,
    input: PathBuf,
    output: PathBuf,
    manifest: PathBuf,
}

#[derive(Debug, PartialEq)]
struct RoutedExpertArgs {
    scenario: PathBuf,
    model: PathBuf,
    layer: u64,
    expert: usize,
    expert_weight: f32,
    input: PathBuf,
    output: PathBuf,
    artifact_dir: PathBuf,
}

#[derive(Debug, PartialEq, Eq)]
struct MoeArgs {
    scenario: PathBuf,
    model: PathBuf,
    layer: u64,
    token: Option<usize>,
    input: PathBuf,
    output: PathBuf,
    artifact_dir: PathBuf,
}

type FfnArgs = MoeArgs;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = match args.first().map(String::as_str) {
        Some("shared-expert") => run_shared_expert(args),
        Some("router") => run_router(args),
        Some("routed-down") => run_routed_down(args),
        Some("routed-expert") => run_routed_expert(args),
        Some("moe") => run_moe(args),
        Some("ffn") => run_ffn(args),
        _ => run(args),
    };
    if let Err(err) = result {
        eprintln!("{err}");
        std::process::exit(1);
    }
}

fn run_ffn<I, S>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = parse_ffn_args(args)?;
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_simpler_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_simpler_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let name = |suffix: &str| format!("blk.{}.{}.weight", args.layer, suffix);
    let hc_function_name = name("hc_ffn_fn");
    let hc_scale_name = name("hc_ffn_scale");
    let hc_base_name = name("hc_ffn_base");
    let ffn_norm_name = name("ffn_norm");
    let router_name = name("ffn_gate_inp");
    let routed_gate_name = name("ffn_gate_exps");
    let routed_up_name = name("ffn_up_exps");
    let routed_down_name = name("ffn_down_exps");
    let shared_gate_name = name("ffn_gate_shexp");
    let shared_up_name = name("ffn_up_shexp");
    let shared_down_name = name("ffn_down_shexp");
    let hc_function = catalog.tensor(&hc_function_name)?;
    let router = catalog.tensor(&router_name)?;
    let routed_gate = catalog.tensor(&routed_gate_name)?;
    let routed_up = catalog.tensor(&routed_up_name)?;
    let routed_down = catalog.tensor(&routed_down_name)?;
    let shared_gate = catalog.tensor(&shared_gate_name)?;
    let shared_up = catalog.tensor(&shared_up_name)?;
    let shared_down = catalog.tensor(&shared_down_name)?;
    if hc_function.tensor_type.name != "f16"
        || router.tensor_type.name != "f16"
        || routed_gate.tensor_type.name != "iq2_xxs"
        || routed_up.tensor_type.name != "iq2_xxs"
        || routed_down.tensor_type.name != "q2_k"
        || shared_gate.tensor_type.name != "q8_0"
        || shared_up.tensor_type.name != "q8_0"
        || shared_down.tensor_type.name != "q8_0"
    {
        return Err("deepseek_ffn_tensor_types_invalid".to_string());
    }
    let decode_f32 = |tensor_name: &str| -> Result<Vec<f32>, String> {
        let tensor = catalog.tensor(tensor_name)?;
        if tensor.tensor_type.name != "f32" {
            return Err(format!(
                "deepseek_ffn_f32_tensor_required:{tensor_name}:{}",
                tensor.tensor_type.name
            ));
        }
        decode_f32_tensor(&catalog.read_tensor(tensor_name)?, &tensor.dimensions)
    };
    let hash_selected = if args.layer < DEEPSEEK_V4_FLASH_PROFILE.num_hash_layers {
        Some(read_hash_router_experts(
            &catalog,
            args.layer,
            args.token.expect("parser requires hash-router token"),
        )?)
    } else {
        None
    };
    let bias_name = name("ffn_exp_probs_b");
    let bias = if catalog.tensors.contains_key(&bias_name) {
        Some(decode_f32(&bias_name)?)
    } else {
        None
    };
    let input = read_f32(&args.input)?;
    let hc_function_payload = catalog.read_tensor(&hc_function_name)?;
    let router_payload = catalog.read_tensor(&router_name)?;
    let routed_gate_payload = catalog.read_tensor(&routed_gate_name)?;
    let routed_up_payload = catalog.read_tensor(&routed_up_name)?;
    let routed_down_payload = catalog.read_tensor(&routed_down_name)?;
    let shared_gate_payload = catalog.read_tensor(&shared_gate_name)?;
    let shared_up_payload = catalog.read_tensor(&shared_up_name)?;
    let shared_down_payload = catalog.read_tensor(&shared_down_name)?;
    let hc_scale = decode_f32(&hc_scale_name)?;
    let hc_base = decode_f32(&hc_base_name)?;
    let ffn_norm = decode_f32(&ffn_norm_name)?;
    let execution = execute_deepseek_ffn_through_simpler(
        &topology,
        &TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 1,
        },
        &args.artifact_dir,
        600_000,
        &DeepseekV4FlashFfnWeights {
            hc_function: &hc_function_payload,
            hc_function_dimensions: &hc_function.dimensions,
            hc_scale: &hc_scale,
            hc_base: &hc_base,
            ffn_norm: &ffn_norm,
            moe: DeepseekV4FlashMoeWeights {
                router: &router_payload,
                router_dimensions: &router.dimensions,
                routed_gate: &routed_gate_payload,
                routed_gate_dimensions: &routed_gate.dimensions,
                routed_up: &routed_up_payload,
                routed_up_dimensions: &routed_up.dimensions,
                routed_down: &routed_down_payload,
                routed_down_dimensions: &routed_down.dimensions,
                shared_gate: &shared_gate_payload,
                shared_gate_dimensions: &shared_gate.dimensions,
                shared_up: &shared_up_payload,
                shared_up_dimensions: &shared_up.dimensions,
                shared_down: &shared_down_payload,
                shared_down_dimensions: &shared_down.dimensions,
            },
        },
        &input,
        bias.as_deref(),
        hash_selected.as_deref(),
        DEEPSEEK_V4_FLASH_PROFILE.hc_mult as usize,
        DEEPSEEK_V4_FLASH_PROFILE.hc_sinkhorn_iters as usize,
        DEEPSEEK_V4_FLASH_RMS_EPS,
        DEEPSEEK_V4_FLASH_SWIGLU_CLAMP,
    )?;
    write_f32(&args.output, &execution.output_hc)?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "model": args.model,
            "layer": args.layer,
            "token": args.token,
            "expert_indices": execution.router.expert_indices,
            "expert_weights": execution.router.expert_weights,
            "output_values": execution.output_hc.len(),
            "output": args.output,
            "backend": "simpler-c-api",
            "operation": "deepseek-ffn",
        })
    );
    Ok(())
}

fn run_moe<I, S>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = parse_moe_args(args)?;
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_simpler_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_simpler_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let name = |suffix: &str| format!("blk.{}.{}.weight", args.layer, suffix);
    let router_name = name("ffn_gate_inp");
    let routed_gate_name = name("ffn_gate_exps");
    let routed_up_name = name("ffn_up_exps");
    let routed_down_name = name("ffn_down_exps");
    let shared_gate_name = name("ffn_gate_shexp");
    let shared_up_name = name("ffn_up_shexp");
    let shared_down_name = name("ffn_down_shexp");
    let router = catalog.tensor(&router_name)?;
    let routed_gate = catalog.tensor(&routed_gate_name)?;
    let routed_up = catalog.tensor(&routed_up_name)?;
    let routed_down = catalog.tensor(&routed_down_name)?;
    let shared_gate = catalog.tensor(&shared_gate_name)?;
    let shared_up = catalog.tensor(&shared_up_name)?;
    let shared_down = catalog.tensor(&shared_down_name)?;
    if router.tensor_type.name != "f16"
        || routed_gate.tensor_type.name != "iq2_xxs"
        || routed_up.tensor_type.name != "iq2_xxs"
        || routed_down.tensor_type.name != "q2_k"
        || shared_gate.tensor_type.name != "q8_0"
        || shared_up.tensor_type.name != "q8_0"
        || shared_down.tensor_type.name != "q8_0"
    {
        return Err("deepseek_moe_tensor_types_invalid".to_string());
    }
    let hash_selected = if args.layer < DEEPSEEK_V4_FLASH_PROFILE.num_hash_layers {
        Some(read_hash_router_experts(
            &catalog,
            args.layer,
            args.token.expect("parser requires hash-router token"),
        )?)
    } else {
        None
    };
    let bias_name = name("ffn_exp_probs_b");
    let bias = if let Some(tensor) = catalog.tensors.get(&bias_name) {
        Some(decode_f32_tensor(
            &catalog.read_tensor(&bias_name)?,
            &tensor.dimensions,
        )?)
    } else {
        None
    };
    let input = read_f32(&args.input)?;
    let router_payload = catalog.read_tensor(&router_name)?;
    let routed_gate_payload = catalog.read_tensor(&routed_gate_name)?;
    let routed_up_payload = catalog.read_tensor(&routed_up_name)?;
    let routed_down_payload = catalog.read_tensor(&routed_down_name)?;
    let shared_gate_payload = catalog.read_tensor(&shared_gate_name)?;
    let shared_up_payload = catalog.read_tensor(&shared_up_name)?;
    let shared_down_payload = catalog.read_tensor(&shared_down_name)?;
    let execution = execute_deepseek_moe_through_simpler(
        &topology,
        &TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 1,
        },
        &args.artifact_dir,
        500_000,
        &DeepseekV4FlashMoeWeights {
            router: &router_payload,
            router_dimensions: &router.dimensions,
            routed_gate: &routed_gate_payload,
            routed_gate_dimensions: &routed_gate.dimensions,
            routed_up: &routed_up_payload,
            routed_up_dimensions: &routed_up.dimensions,
            routed_down: &routed_down_payload,
            routed_down_dimensions: &routed_down.dimensions,
            shared_gate: &shared_gate_payload,
            shared_gate_dimensions: &shared_gate.dimensions,
            shared_up: &shared_up_payload,
            shared_up_dimensions: &shared_up.dimensions,
            shared_down: &shared_down_payload,
            shared_down_dimensions: &shared_down.dimensions,
        },
        &input,
        bias.as_deref(),
        hash_selected.as_deref(),
        DEEPSEEK_V4_FLASH_SWIGLU_CLAMP,
    )?;
    write_f32(&args.output, &execution.output)?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "model": args.model,
            "layer": args.layer,
            "token": args.token,
            "expert_indices": execution.router.expert_indices,
            "expert_weights": execution.router.expert_weights,
            "output_values": execution.output.len(),
            "output": args.output,
            "backend": "simpler-c-api",
            "operation": "deepseek-moe",
        })
    );
    Ok(())
}

fn run_routed_expert<I, S>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = parse_routed_expert_args(args)?;
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_simpler_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_simpler_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let tensor_name = |kind: &str| format!("blk.{}.ffn_{}_exps.weight", args.layer, kind);
    let gate_name = tensor_name("gate");
    let up_name = tensor_name("up");
    let down_name = tensor_name("down");
    let gate = catalog.tensor(&gate_name)?;
    let up = catalog.tensor(&up_name)?;
    let down = catalog.tensor(&down_name)?;
    if gate.tensor_type.name != "iq2_xxs"
        || up.tensor_type.name != "iq2_xxs"
        || down.tensor_type.name != "q2_k"
    {
        return Err(format!(
            "deepseek_routed_expert_tensor_types_invalid:gate={}:up={}:down={}",
            gate.tensor_type.name, up.tensor_type.name, down.tensor_type.name
        ));
    }
    let input = read_f32(&args.input)?;
    let output = execute_deepseek_routed_expert_through_simpler(
        &topology,
        &TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 1,
        },
        &args.artifact_dir,
        400_000,
        &catalog.read_tensor(&gate_name)?,
        &gate.dimensions,
        &catalog.read_tensor(&up_name)?,
        &up.dimensions,
        &catalog.read_tensor(&down_name)?,
        &down.dimensions,
        args.expert,
        args.expert_weight,
        &input,
        DEEPSEEK_V4_FLASH_SWIGLU_CLAMP,
    )?;
    write_f32(&args.output, &output)?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "model": args.model,
            "layer": args.layer,
            "expert": args.expert,
            "expert_weight": args.expert_weight,
            "input_values": input.len(),
            "output_values": output.len(),
            "output": args.output,
            "backend": "simpler-c-api",
            "operation": "deepseek-routed-expert",
        })
    );
    Ok(())
}

fn run_routed_down<I, S>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = parse_routed_down_args(args)?;
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_simpler_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_simpler_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let tensor_name = format!("blk.{}.ffn_down_exps.weight", args.layer);
    let tensor = catalog.tensor(&tensor_name)?;
    if tensor.tensor_type.name != "q2_k" {
        return Err(format!(
            "deepseek_routed_down_q2_k_tensor_required:{}:{}",
            tensor.name, tensor.tensor_type.name
        ));
    }
    let input = read_f32(&args.input)?;
    let output = execute_deepseek_q2_k_expert_projection_through_simpler(
        &topology,
        &TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 1,
        },
        &args.manifest,
        300_000,
        &catalog.read_tensor(&tensor_name)?,
        &tensor.dimensions,
        args.expert,
        &input,
    )?;
    write_f32(&args.output, &output)?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "model": args.model,
            "layer": args.layer,
            "expert": args.expert,
            "input_values": input.len(),
            "output_values": output.len(),
            "output": args.output,
            "backend": "simpler-c-api",
            "operation": "deepseek-routed-down-q2-k",
        })
    );
    Ok(())
}

fn run_router<I, S>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = parse_router_args(args)?;
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_simpler_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_simpler_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let router_name = format!("blk.{}.ffn_gate_inp.weight", args.layer);
    let router = catalog.tensor(&router_name)?;
    if router.tensor_type.name != "f16" {
        return Err(format!(
            "deepseek_router_f16_tensor_required:{}:{}",
            router.name, router.tensor_type.name
        ));
    }
    let selected = if args.layer < DEEPSEEK_V4_FLASH_PROFILE.num_hash_layers {
        Some(read_hash_router_experts(
            &catalog,
            args.layer,
            args.token.expect("parser requires hash-router token"),
        )?)
    } else {
        None
    };
    let bias_name = format!("blk.{}.ffn_exp_probs_b.weight", args.layer);
    let bias = if let Some(tensor) = catalog.tensors.get(&bias_name) {
        Some(decode_f32_tensor(
            &catalog.read_tensor(&bias_name)?,
            &tensor.dimensions,
        )?)
    } else {
        None
    };
    let input = read_f32(&args.input)?;
    let output = execute_deepseek_router_through_simpler(
        &topology,
        &TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 1,
        },
        &args.manifest,
        &catalog.read_tensor(&router_name)?,
        &router.dimensions,
        &input,
        bias.as_deref(),
        selected.as_deref(),
    )?;
    let json = serde_json::json!({
        "status": "ok",
        "model": args.model,
        "layer": args.layer,
        "token": args.token,
        "expert_indices": output.expert_indices,
        "expert_weights": output.expert_weights,
        "probabilities": output.probabilities,
        "backend": "simpler-c-api",
        "operation": "deepseek-router",
    });
    fs::write(
        &args.output,
        serde_json::to_vec_pretty(&json).map_err(|err| format!("json_encode_failed:{err}"))?,
    )
    .map_err(|err| {
        format!(
            "deepseek_router_output_write_failed:{}:{err}",
            args.output.display()
        )
    })?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "layer": args.layer,
            "token": args.token,
            "expert_indices": json["expert_indices"],
            "expert_weights": json["expert_weights"],
            "output": args.output,
            "backend": "simpler-c-api",
        })
    );
    Ok(())
}

fn read_hash_router_experts(
    catalog: &GgufCatalog,
    layer: u64,
    token: usize,
) -> Result<Vec<usize>, String> {
    let name = format!("blk.{layer}.ffn_gate_tid2eid.weight");
    let tensor = catalog.tensor(&name)?;
    let top_k = DEEPSEEK_V4_FLASH_PROFILE.num_experts_used as usize;
    if tensor.tensor_type.name != "i32"
        || tensor.dimensions.first().copied() != Some(top_k as u64)
        || token as u64 >= tensor.dimensions.get(1).copied().unwrap_or(0)
    {
        return Err(format!(
            "deepseek_hash_router_table_invalid:{name}:token={token}:dimensions={:?}:type={}",
            tensor.dimensions, tensor.tensor_type.name
        ));
    }
    let payload = catalog.read_tensor(&name)?;
    let offset = token
        .checked_mul(top_k)
        .and_then(|value| value.checked_mul(4))
        .ok_or_else(|| "deepseek hash router offset overflow".to_string())?;
    payload[offset..offset + top_k * 4]
        .chunks_exact(4)
        .map(|chunk| {
            let expert = i32::from_le_bytes(chunk.try_into().expect("four-byte chunk"));
            usize::try_from(expert)
                .map_err(|_| format!("deepseek_hash_router_expert_invalid:{expert}"))
        })
        .collect()
}

fn run_shared_expert<I, S>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = parse_shared_expert_args(args)?;
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_simpler_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_simpler_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let tensor_name = |kind: &str| format!("blk.{}.ffn_{}.weight", args.layer, kind);
    let gate_name = tensor_name("gate_shexp");
    let up_name = tensor_name("up_shexp");
    let down_name = tensor_name("down_shexp");
    let gate = catalog.tensor(&gate_name)?;
    let up = catalog.tensor(&up_name)?;
    let down = catalog.tensor(&down_name)?;
    for tensor in [gate, up, down] {
        if tensor.tensor_type.name != "q8_0" {
            return Err(format!(
                "deepseek_shared_expert_q8_tensor_required:{}:{}",
                tensor.name, tensor.tensor_type.name
            ));
        }
    }
    let input = read_f32(&args.input)?;
    let output = execute_deepseek_shared_expert_through_simpler(
        &topology,
        &TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 1,
        },
        &args.artifact_dir,
        200_000,
        &catalog.read_tensor(&gate_name)?,
        &gate.dimensions,
        &catalog.read_tensor(&up_name)?,
        &up.dimensions,
        &catalog.read_tensor(&down_name)?,
        &down.dimensions,
        &input,
        DEEPSEEK_V4_FLASH_SWIGLU_CLAMP,
    )?;
    write_f32(&args.output, &output)?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "model": args.model,
            "layer": args.layer,
            "input_values": input.len(),
            "output_values": output.len(),
            "output": args.output,
            "backend": "simpler-c-api",
            "operation": "deepseek-shared-expert",
        })
    );
    Ok(())
}

fn run<I, S>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = parse_args(args)?;
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_simpler_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_simpler_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let tensor = catalog.tensor(&args.tensor)?;
    if tensor.tensor_type.name != "q8_0" {
        return Err(format!(
            "deepseek_simpler_q8_tensor_required:{}:{}",
            args.tensor, tensor.tensor_type.name
        ));
    }
    let input = read_f32(&args.input)?;
    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels: [0; 8] },
        scope_depth: 0,
        task_id: 1,
    };
    let weight = catalog.read_tensor(&args.tensor)?;
    let output = if args.groups == 1 {
        execute_deepseek_q8_projection_through_simpler(
            &topology,
            &task,
            &args.manifest,
            100_000,
            &weight,
            &tensor.dimensions,
            &input,
        )?
    } else {
        execute_deepseek_grouped_q8_projection_through_simpler(
            &topology,
            &task,
            &args.manifest,
            100_000,
            &weight,
            &tensor.dimensions,
            &input,
            args.groups,
        )?
    };
    write_f32(&args.output, &output)?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "model": args.model,
            "tensor": args.tensor,
            "input_values": input.len(),
            "output_values": output.len(),
            "groups": args.groups,
            "output": args.output,
            "backend": "simpler-c-api",
        })
    );
    Ok(())
}

fn parse_shared_expert_args<I, S>(args: I) -> Result<SharedExpertArgs, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args: Vec<String> = args.into_iter().map(Into::into).collect();
    if args.first().map(String::as_str) != Some("shared-expert") {
        return Err(
            "usage: deepseek-v4-flash-simpler shared-expert --model FILE --layer N --input FILE --output FILE [--scenario FILE] [--artifact-dir DIR]"
                .to_string(),
        );
    }
    let mut options = BTreeMap::new();
    let mut index = 1;
    while index < args.len() {
        let option = &args[index];
        if !option.starts_with("--") || index + 1 >= args.len() {
            return Err(format!("invalid_option:{option}"));
        }
        if options
            .insert(option.clone(), args[index + 1].clone())
            .is_some()
        {
            return Err(format!("duplicate_option:{option}"));
        }
        index += 2;
    }
    if let Some(option) = options.keys().find(|name| {
        !matches!(
            name.as_str(),
            "--scenario" | "--model" | "--layer" | "--input" | "--output" | "--artifact-dir"
        )
    }) {
        return Err(format!("unknown_option:{option}"));
    }
    let required = |name: &str| {
        options
            .get(name)
            .cloned()
            .ok_or_else(|| format!("required_option_missing:{name}"))
    };
    let layer_text = required("--layer")?;
    let layer = layer_text
        .parse::<u64>()
        .map_err(|_| format!("invalid_layer:{layer_text}"))?;
    if layer >= DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!("invalid_layer:{layer}"));
    }
    Ok(SharedExpertArgs {
        scenario: options
            .get("--scenario")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
        model: PathBuf::from(required("--model")?),
        layer,
        input: PathBuf::from(required("--input")?),
        output: PathBuf::from(required("--output")?),
        artifact_dir: options
            .get("--artifact-dir")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                std::env::temp_dir().join("simpler-deepseek-shared-expert-artifacts")
            }),
    })
}

fn parse_router_args<I, S>(args: I) -> Result<RouterArgs, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args: Vec<String> = args.into_iter().map(Into::into).collect();
    if args.first().map(String::as_str) != Some("router") {
        return Err(
            "usage: deepseek-v4-flash-simpler router --model FILE --layer N --input FILE --output FILE [--token N] [--scenario FILE] [--manifest FILE]"
                .to_string(),
        );
    }
    let mut options = BTreeMap::new();
    let mut index = 1;
    while index < args.len() {
        let option = &args[index];
        if !option.starts_with("--") || index + 1 >= args.len() {
            return Err(format!("invalid_option:{option}"));
        }
        if options
            .insert(option.clone(), args[index + 1].clone())
            .is_some()
        {
            return Err(format!("duplicate_option:{option}"));
        }
        index += 2;
    }
    if let Some(option) = options.keys().find(|name| {
        !matches!(
            name.as_str(),
            "--scenario"
                | "--model"
                | "--layer"
                | "--token"
                | "--input"
                | "--output"
                | "--manifest"
        )
    }) {
        return Err(format!("unknown_option:{option}"));
    }
    let required = |name: &str| {
        options
            .get(name)
            .cloned()
            .ok_or_else(|| format!("required_option_missing:{name}"))
    };
    let layer_text = required("--layer")?;
    let layer = layer_text
        .parse::<u64>()
        .map_err(|_| format!("invalid_layer:{layer_text}"))?;
    if layer >= DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!("invalid_layer:{layer}"));
    }
    let token = options
        .get("--token")
        .map(|value| {
            value
                .parse::<usize>()
                .map_err(|_| format!("invalid_token:{value}"))
        })
        .transpose()?;
    if layer < DEEPSEEK_V4_FLASH_PROFILE.num_hash_layers && token.is_none() {
        return Err(format!("hash_router_token_required:layer={layer}"));
    }
    Ok(RouterArgs {
        scenario: options
            .get("--scenario")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
        model: PathBuf::from(required("--model")?),
        layer,
        token,
        input: PathBuf::from(required("--input")?),
        output: PathBuf::from(required("--output")?),
        manifest: options
            .get("--manifest")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                std::env::temp_dir()
                    .join("simpler-deepseek-router-artifacts")
                    .join("host_fp32_gemm_manifest.json")
            }),
    })
}

fn parse_moe_args<I, S>(args: I) -> Result<MoeArgs, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args: Vec<String> = args.into_iter().map(Into::into).collect();
    if args.first().map(String::as_str) != Some("moe") {
        return Err(
            "usage: deepseek-v4-flash-simpler moe --model FILE --layer N --input FILE --output FILE [--token N] [--scenario FILE] [--artifact-dir DIR]"
                .to_string(),
        );
    }
    let mut options = BTreeMap::new();
    let mut index = 1;
    while index < args.len() {
        let option = &args[index];
        if !option.starts_with("--") || index + 1 >= args.len() {
            return Err(format!("invalid_option:{option}"));
        }
        if options
            .insert(option.clone(), args[index + 1].clone())
            .is_some()
        {
            return Err(format!("duplicate_option:{option}"));
        }
        index += 2;
    }
    if let Some(option) = options.keys().find(|name| {
        !matches!(
            name.as_str(),
            "--scenario"
                | "--model"
                | "--layer"
                | "--token"
                | "--input"
                | "--output"
                | "--artifact-dir"
        )
    }) {
        return Err(format!("unknown_option:{option}"));
    }
    let required = |name: &str| {
        options
            .get(name)
            .cloned()
            .ok_or_else(|| format!("required_option_missing:{name}"))
    };
    let layer_text = required("--layer")?;
    let layer = layer_text
        .parse::<u64>()
        .map_err(|_| format!("invalid_layer:{layer_text}"))?;
    if layer >= DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!("invalid_layer:{layer}"));
    }
    let token = options
        .get("--token")
        .map(|value| {
            value
                .parse::<usize>()
                .map_err(|_| format!("invalid_token:{value}"))
        })
        .transpose()?;
    if layer < DEEPSEEK_V4_FLASH_PROFILE.num_hash_layers && token.is_none() {
        return Err(format!("hash_router_token_required:layer={layer}"));
    }
    Ok(MoeArgs {
        scenario: options
            .get("--scenario")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
        model: PathBuf::from(required("--model")?),
        layer,
        token,
        input: PathBuf::from(required("--input")?),
        output: PathBuf::from(required("--output")?),
        artifact_dir: options
            .get("--artifact-dir")
            .map(PathBuf::from)
            .unwrap_or_else(|| std::env::temp_dir().join("simpler-deepseek-moe-artifacts")),
    })
}

fn parse_ffn_args<I, S>(args: I) -> Result<FfnArgs, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let mut args: Vec<String> = args.into_iter().map(Into::into).collect();
    if args.first().map(String::as_str) != Some("ffn") {
        return Err(
            "usage: deepseek-v4-flash-simpler ffn --model FILE --layer N --input FILE --output FILE [--token N] [--scenario FILE] [--artifact-dir DIR]"
                .to_string(),
        );
    }
    let has_artifact_dir = args.iter().any(|arg| arg == "--artifact-dir");
    args[0] = "moe".to_string();
    let mut parsed = parse_moe_args(args).map_err(|err| {
        err.replace(
            "usage: deepseek-v4-flash-simpler moe",
            "usage: deepseek-v4-flash-simpler ffn",
        )
    })?;
    if !has_artifact_dir {
        parsed.artifact_dir = std::env::temp_dir().join("simpler-deepseek-ffn-artifacts");
    }
    Ok(parsed)
}

fn parse_routed_down_args<I, S>(args: I) -> Result<RoutedDownArgs, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args: Vec<String> = args.into_iter().map(Into::into).collect();
    if args.first().map(String::as_str) != Some("routed-down") {
        return Err(
            "usage: deepseek-v4-flash-simpler routed-down --model FILE --layer N --expert N --input FILE --output FILE [--scenario FILE] [--manifest FILE]"
                .to_string(),
        );
    }
    let mut options = BTreeMap::new();
    let mut index = 1;
    while index < args.len() {
        let option = &args[index];
        if !option.starts_with("--") || index + 1 >= args.len() {
            return Err(format!("invalid_option:{option}"));
        }
        if options
            .insert(option.clone(), args[index + 1].clone())
            .is_some()
        {
            return Err(format!("duplicate_option:{option}"));
        }
        index += 2;
    }
    if let Some(option) = options.keys().find(|name| {
        !matches!(
            name.as_str(),
            "--scenario"
                | "--model"
                | "--layer"
                | "--expert"
                | "--input"
                | "--output"
                | "--manifest"
        )
    }) {
        return Err(format!("unknown_option:{option}"));
    }
    let required = |name: &str| {
        options
            .get(name)
            .cloned()
            .ok_or_else(|| format!("required_option_missing:{name}"))
    };
    let layer_text = required("--layer")?;
    let layer = layer_text
        .parse::<u64>()
        .map_err(|_| format!("invalid_layer:{layer_text}"))?;
    if layer >= DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!("invalid_layer:{layer}"));
    }
    let expert_text = required("--expert")?;
    let expert = expert_text
        .parse::<usize>()
        .map_err(|_| format!("invalid_expert:{expert_text}"))?;
    if expert >= DEEPSEEK_V4_FLASH_PROFILE.num_experts as usize {
        return Err(format!("invalid_expert:{expert}"));
    }
    Ok(RoutedDownArgs {
        scenario: options
            .get("--scenario")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
        model: PathBuf::from(required("--model")?),
        layer,
        expert,
        input: PathBuf::from(required("--input")?),
        output: PathBuf::from(required("--output")?),
        manifest: options
            .get("--manifest")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                std::env::temp_dir()
                    .join("simpler-deepseek-routed-down-artifacts")
                    .join("host_q8_block_dot_manifest.json")
            }),
    })
}

fn parse_routed_expert_args<I, S>(args: I) -> Result<RoutedExpertArgs, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args: Vec<String> = args.into_iter().map(Into::into).collect();
    if args.first().map(String::as_str) != Some("routed-expert") {
        return Err(
            "usage: deepseek-v4-flash-simpler routed-expert --model FILE --layer N --expert N --expert-weight F --input FILE --output FILE [--scenario FILE] [--artifact-dir DIR]"
                .to_string(),
        );
    }
    let mut options = BTreeMap::new();
    let mut index = 1;
    while index < args.len() {
        let option = &args[index];
        if !option.starts_with("--") || index + 1 >= args.len() {
            return Err(format!("invalid_option:{option}"));
        }
        if options
            .insert(option.clone(), args[index + 1].clone())
            .is_some()
        {
            return Err(format!("duplicate_option:{option}"));
        }
        index += 2;
    }
    if let Some(option) = options.keys().find(|name| {
        !matches!(
            name.as_str(),
            "--scenario"
                | "--model"
                | "--layer"
                | "--expert"
                | "--expert-weight"
                | "--input"
                | "--output"
                | "--artifact-dir"
        )
    }) {
        return Err(format!("unknown_option:{option}"));
    }
    let required = |name: &str| {
        options
            .get(name)
            .cloned()
            .ok_or_else(|| format!("required_option_missing:{name}"))
    };
    let layer_text = required("--layer")?;
    let layer = layer_text
        .parse::<u64>()
        .map_err(|_| format!("invalid_layer:{layer_text}"))?;
    if layer >= DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!("invalid_layer:{layer}"));
    }
    let expert_text = required("--expert")?;
    let expert = expert_text
        .parse::<usize>()
        .map_err(|_| format!("invalid_expert:{expert_text}"))?;
    if expert >= DEEPSEEK_V4_FLASH_PROFILE.num_experts as usize {
        return Err(format!("invalid_expert:{expert}"));
    }
    let expert_weight_text = required("--expert-weight")?;
    let expert_weight = expert_weight_text
        .parse::<f32>()
        .ok()
        .filter(|value| value.is_finite())
        .ok_or_else(|| format!("invalid_expert_weight:{expert_weight_text}"))?;
    Ok(RoutedExpertArgs {
        scenario: options
            .get("--scenario")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
        model: PathBuf::from(required("--model")?),
        layer,
        expert,
        expert_weight,
        input: PathBuf::from(required("--input")?),
        output: PathBuf::from(required("--output")?),
        artifact_dir: options
            .get("--artifact-dir")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                std::env::temp_dir().join("simpler-deepseek-routed-expert-artifacts")
            }),
    })
}

fn write_f32(path: &PathBuf, values: &[f32]) -> Result<(), String> {
    let bytes: Vec<u8> = values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect();
    fs::write(path, bytes).map_err(|err| {
        format!(
            "deepseek_simpler_output_write_failed:{}:{err}",
            path.display()
        )
    })
}

fn parse_args<I, S>(args: I) -> Result<ProjectQ8Args, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args: Vec<String> = args.into_iter().map(Into::into).collect();
    if args.first().map(String::as_str) != Some("project-q8") {
        return Err(
            "usage: deepseek-v4-flash-simpler project-q8 --model FILE --tensor NAME --input FILE --output FILE [--groups N] [--scenario FILE] [--manifest FILE]"
                .to_string(),
        );
    }
    let mut options = BTreeMap::new();
    let mut index = 1;
    while index < args.len() {
        let option = &args[index];
        if !option.starts_with("--") || index + 1 >= args.len() {
            return Err(format!("invalid_option:{option}"));
        }
        if options
            .insert(option.clone(), args[index + 1].clone())
            .is_some()
        {
            return Err(format!("duplicate_option:{option}"));
        }
        index += 2;
    }
    let required = |name: &str| {
        options
            .get(name)
            .cloned()
            .ok_or_else(|| format!("required_option_missing:{name}"))
    };
    let unknown = options.keys().find(|name| {
        !matches!(
            name.as_str(),
            "--scenario"
                | "--model"
                | "--tensor"
                | "--input"
                | "--output"
                | "--manifest"
                | "--groups"
        )
    });
    if let Some(option) = unknown {
        return Err(format!("unknown_option:{option}"));
    }
    let groups = options
        .get("--groups")
        .map(|value| {
            value
                .parse::<usize>()
                .map_err(|_| format!("invalid_groups:{value}"))
        })
        .transpose()?
        .unwrap_or(1);
    if groups == 0 {
        return Err("invalid_groups:0".to_string());
    }
    Ok(ProjectQ8Args {
        scenario: options
            .get("--scenario")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
        model: PathBuf::from(required("--model")?),
        tensor: required("--tensor")?,
        input: PathBuf::from(required("--input")?),
        output: PathBuf::from(required("--output")?),
        manifest: options
            .get("--manifest")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                std::env::temp_dir()
                    .join("simpler-deepseek-q8-project-artifacts")
                    .join("host_q8_block_dot_manifest.json")
            }),
        groups,
    })
}

fn read_f32(path: &PathBuf) -> Result<Vec<f32>, String> {
    let bytes = fs::read(path).map_err(|err| {
        format!(
            "deepseek_simpler_input_read_failed:{}:{err}",
            path.display()
        )
    })?;
    if bytes.len() % 4 != 0 {
        return Err(format!(
            "deepseek_simpler_input_unaligned:{}:{}",
            path.display(),
            bytes.len()
        ));
    }
    Ok(bytes
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes(chunk.try_into().expect("four-byte chunk")))
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn router_args_distinguish_hash_and_topk_layers() {
        let hash = parse_router_args([
            "router",
            "--model",
            "model.gguf",
            "--layer",
            "0",
            "--token",
            "108149",
            "--input",
            "input.f32",
            "--output",
            "router.json",
        ])
        .expect("parse hash router");
        assert_eq!(hash.token, Some(108_149));

        let topk = parse_router_args([
            "router",
            "--model",
            "model.gguf",
            "--layer",
            "3",
            "--input",
            "input.f32",
            "--output",
            "router.json",
        ])
        .expect("parse top-k router");
        assert_eq!(topk.token, None);
    }

    #[test]
    fn router_args_require_token_for_hash_layers() {
        let err = parse_router_args([
            "router",
            "--model",
            "model.gguf",
            "--layer",
            "2",
            "--input",
            "input.f32",
            "--output",
            "router.json",
        ])
        .expect_err("reject hash router without token");
        assert_eq!(err, "hash_router_token_required:layer=2");
    }

    #[test]
    fn moe_args_distinguish_hash_and_topk_layers() {
        let hash = parse_moe_args([
            "moe",
            "--model",
            "model.gguf",
            "--layer",
            "0",
            "--token",
            "108149",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect("parse hash MoE command");
        assert_eq!(hash.token, Some(108_149));

        let topk = parse_moe_args([
            "moe",
            "--model",
            "model.gguf",
            "--layer",
            "3",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect("parse top-k MoE command");
        assert_eq!(topk.token, None);
        assert!(topk
            .artifact_dir
            .ends_with("simpler-deepseek-moe-artifacts"));
    }

    #[test]
    fn shared_expert_args_accept_real_layer() {
        let args = parse_shared_expert_args([
            "shared-expert",
            "--model",
            "model.gguf",
            "--layer",
            "42",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect("parse shared expert command");
        assert_eq!(args.layer, 42);
        assert!(args
            .artifact_dir
            .ends_with("simpler-deepseek-shared-expert-artifacts"));
    }

    #[test]
    fn shared_expert_args_reject_layer_outside_model() {
        let err = parse_shared_expert_args([
            "shared-expert",
            "--model",
            "model.gguf",
            "--layer",
            "43",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect_err("reject layer outside model");
        assert_eq!(err, "invalid_layer:43");
    }

    #[test]
    fn ffn_args_distinguish_hash_and_topk_layers() {
        let hash = parse_ffn_args([
            "ffn",
            "--model",
            "model.gguf",
            "--layer",
            "0",
            "--token",
            "108149",
            "--input",
            "residual-hc.f32",
            "--output",
            "output-hc.f32",
        ])
        .expect("parse hash-layer FFN");
        assert_eq!(hash.token, Some(108_149));

        let topk = parse_ffn_args([
            "ffn",
            "--model",
            "model.gguf",
            "--layer",
            "3",
            "--input",
            "residual-hc.f32",
            "--output",
            "output-hc.f32",
        ])
        .expect("parse top-k FFN");
        assert_eq!(topk.token, None);
        assert!(topk
            .artifact_dir
            .ends_with("simpler-deepseek-ffn-artifacts"));
    }

    #[test]
    fn ffn_args_require_token_for_hash_layers() {
        let err = parse_ffn_args([
            "ffn",
            "--model",
            "model.gguf",
            "--layer",
            "2",
            "--input",
            "residual-hc.f32",
            "--output",
            "output-hc.f32",
        ])
        .expect_err("reject hash-layer FFN without token");
        assert_eq!(err, "hash_router_token_required:layer=2");
    }

    #[test]
    fn routed_down_args_validate_layer_and_expert() {
        let args = parse_routed_down_args([
            "routed-down",
            "--model",
            "model.gguf",
            "--layer",
            "42",
            "--expert",
            "255",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect("parse routed down command");
        assert_eq!(args.layer, 42);
        assert_eq!(args.expert, 255);
        assert!(args.manifest.ends_with("host_q8_block_dot_manifest.json"));

        let err = parse_routed_down_args([
            "routed-down",
            "--model",
            "model.gguf",
            "--layer",
            "0",
            "--expert",
            "256",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect_err("reject expert outside model");
        assert_eq!(err, "invalid_expert:256");
    }

    #[test]
    fn routed_expert_args_require_finite_router_weight() {
        let args = parse_routed_expert_args([
            "routed-expert",
            "--model",
            "model.gguf",
            "--layer",
            "3",
            "--expert",
            "79",
            "--expert-weight",
            "0.3125",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect("parse routed expert command");
        assert_eq!(args.expert, 79);
        assert_eq!(args.expert_weight, 0.3125);

        let err = parse_routed_expert_args([
            "routed-expert",
            "--model",
            "model.gguf",
            "--layer",
            "3",
            "--expert",
            "79",
            "--expert-weight",
            "NaN",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect_err("reject non-finite router weight");
        assert_eq!(err, "invalid_expert_weight:NaN");
    }

    #[test]
    fn project_q8_args_have_reusable_defaults() {
        let args = parse_args([
            "project-q8",
            "--model",
            "model.gguf",
            "--tensor",
            "blk.0.attn_q_a.weight",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect("parse project-q8 command");
        assert_eq!(
            args.scenario,
            PathBuf::from("scenarios/mvp_2host_single_domain.yaml")
        );
        assert!(args.manifest.ends_with("host_q8_block_dot_manifest.json"));
        assert_eq!(args.groups, 1);
    }

    #[test]
    fn project_q8_args_accept_grouped_projection() {
        let args = parse_args([
            "project-q8",
            "--model",
            "model.gguf",
            "--tensor",
            "blk.0.attn_output_a.weight",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
            "--groups",
            "8",
        ])
        .expect("parse grouped project-q8 command");
        assert_eq!(args.groups, 8);
    }

    #[test]
    fn project_q8_args_reject_zero_groups() {
        let err = parse_args([
            "project-q8",
            "--model",
            "model.gguf",
            "--tensor",
            "tensor",
            "--input",
            "input",
            "--output",
            "output",
            "--groups",
            "0",
        ])
        .expect_err("reject zero groups");
        assert_eq!(err, "invalid_groups:0");
    }

    #[test]
    fn project_q8_args_reject_unknown_options() {
        let err = parse_args([
            "project-q8",
            "--model",
            "model.gguf",
            "--tensor",
            "tensor",
            "--input",
            "input",
            "--output",
            "output",
            "--bogus",
            "value",
        ])
        .expect_err("reject unknown option");
        assert_eq!(err, "unknown_option:--bogus");
    }
}
