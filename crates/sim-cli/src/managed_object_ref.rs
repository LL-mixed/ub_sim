use sim_services::managed_object_ref::ManagedObjectRef;

fn decode_hex(input: &str) -> anyhow::Result<ManagedObjectRef> {
    if input.len() != ManagedObjectRef::BYTE_LEN * 2
        || !input.bytes().all(|byte| byte.is_ascii_hexdigit())
    {
        anyhow::bail!("expected exactly 512 hexadecimal characters");
    }
    let bytes = (0..input.len())
        .step_by(2)
        .map(|offset| u8::from_str_radix(&input[offset..offset + 2], 16))
        .collect::<Result<Vec<_>, _>>()?;
    ManagedObjectRef::from_le_bytes(&bytes).map_err(anyhow::Error::msg)
}

pub fn run_if_requested() -> anyhow::Result<Option<()>> {
    let mut args = std::env::args_os().skip(1);
    if args.next().as_deref() != Some(std::ffi::OsStr::new("managed-object-ref-inspect")) {
        return Ok(None);
    }
    if args.next().as_deref() != Some(std::ffi::OsStr::new("--hex")) {
        anyhow::bail!("usage: managed-object-ref-inspect --hex <512 hex characters>");
    }
    let encoded = args
        .next()
        .ok_or_else(|| anyhow::anyhow!("--hex requires a value"))?;
    if args.next().is_some() {
        anyhow::bail!("unexpected argument");
    }
    let reference = decode_hex(
        encoded
            .to_str()
            .ok_or_else(|| anyhow::anyhow!("invalid hex text"))?,
    )?;
    // Raw key bytes preserve the C SDK's non-UTF-8 token contract.
    println!(
        "{}",
        serde_json::to_string_pretty(&serde_json::json!({
            "structurally_valid": true, "access_granted": false, "wire_version": 2,
            "wire_bytes": ManagedObjectRef::BYTE_LEN,
            "allocation_key_bytes": reference.allocation_key(),
            "home_node_bytes": reference.home_node(),
            "allocation_generation": reference.generation(),
            "provider_incarnation": reference.incarnation(),
            "object_kind": reference.object_kind(), "object_version": reference.object_version(),
            "owner_entity": reference.owner_entity(), "producer_entity": reference.producer_entity(),
            "access": reference.access(), "allocation_bytes": reference.allocation_bytes(),
            "payload_offset": reference.payload_offset(), "payload_bytes": reference.payload_bytes(),
            "payload_checksum": reference.payload_checksum(),
        }))?
    );
    Ok(Some(()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_invalid_hex_and_unknown_wire() {
        for text in [
            "".to_string(),
            "0".repeat(128),
            "z".repeat(512),
            "+0".repeat(256),
            "é".repeat(256),
            "0".repeat(512),
        ] {
            assert!(decode_hex(&text).is_err());
        }
    }
}
