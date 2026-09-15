//! Lossless Memory Service V2 references. Decoding does not grant access.

/// The managed SDK's mem_service_provider_checksum64 contract. This seed and
/// byte-only update differ from both the W5 positional checksum and FNV-1a's
/// commonly quoted offset basis. Do not substitute a model checksum here.
pub fn payload_checksum(bytes: &[u8]) -> u64 {
    bytes
        .iter()
        .fold(1_469_598_103_934_665_603u64, |hash, byte| {
            (hash ^ u64::from(*byte)).wrapping_mul(1_099_511_628_211)
        })
}

#[derive(Clone, Debug, Eq, PartialEq, Hash)]
pub struct ManagedObjectRef {
    bytes: [u8; Self::BYTE_LEN],
}

impl ManagedObjectRef {
    pub const BYTE_LEN: usize = 256;

    pub fn from_le_bytes(input: &[u8]) -> Result<Self, &'static str> {
        let bytes = input.try_into().map_err(|_| "managed_ref_bad_length")?;
        let reference = Self { bytes };
        if reference.number(0, 8) != 0x514f_424d_4d52_4546 {
            return Err("managed_ref_bad_magic");
        }
        if reference.number(8, 2) != 2 {
            return Err("managed_ref_bad_version");
        }
        if reference.number(64, 4) != Self::BYTE_LEN as u64 {
            return Err("managed_ref_bad_wire_length");
        }
        if reference.number(14, 2) != 0
            || reference.object_kind() == 0
            || reference.object_version() == 0
            || reference.number(12, 2) != 2
            || reference.generation() == 0
            || reference.incarnation() == 0
            || reference.access() == 0
            || reference.access() & !3 != 0
        {
            return Err("managed_ref_invalid_identity");
        }
        let key = token(&reference.bytes[96..192])?;
        token(&reference.bytes[192..256])?;
        let hash = key.iter().fold(1_469_598_103_934_665_603u64, |hash, byte| {
            (hash ^ u64::from(*byte)).wrapping_mul(1_099_511_628_211)
        });
        if hash != reference.number(32, 8) {
            return Err("managed_ref_key_hash_mismatch");
        }
        if reference.allocation_bytes() == 0
            || reference.payload_bytes() == 0
            || reference.payload_offset() > reference.allocation_bytes()
            || reference.payload_bytes() > reference.allocation_bytes() - reference.payload_offset()
        {
            return Err("managed_ref_out_of_bounds");
        }
        Ok(reference)
    }

    pub fn as_le_bytes(&self) -> &[u8; Self::BYTE_LEN] {
        &self.bytes
    }

    pub fn allocation_key(&self) -> &[u8] {
        token(&self.bytes[96..192]).expect("validated immutable key")
    }

    pub fn home_node(&self) -> &[u8] {
        token(&self.bytes[192..256]).expect("validated immutable home")
    }

    fn number(&self, offset: usize, width: usize) -> u64 {
        self.bytes[offset..offset + width]
            .iter()
            .enumerate()
            .fold(0, |value, (index, byte)| {
                value | (u64::from(*byte) << (index * 8))
            })
    }
}

fn token(bytes: &[u8]) -> Result<&[u8], &'static str> {
    let length = bytes
        .iter()
        .position(|byte| *byte == 0)
        .ok_or("managed_ref_unterminated_token")?;
    if length == 0
        || bytes[..length]
            .iter()
            .any(|byte| *byte <= 0x20 || *byte == 0x7f || *byte == b'=')
        || bytes[length..].iter().any(|byte| *byte != 0)
    {
        return Err("managed_ref_noncanonical_token");
    }
    Ok(&bytes[..length])
}

macro_rules! field {
    ($($name:ident, $offset:literal, $width:literal;)+) => {
        impl ManagedObjectRef {
            $(pub fn $name(&self) -> u64 { self.number($offset, $width) })+
        }
    };
}

field! {
    object_kind, 10, 2;
    owner_entity, 16, 4;
    producer_entity, 20, 4;
    object_version, 24, 8;
    payload_offset, 40, 8;
    payload_bytes, 48, 8;
    payload_checksum, 56, 8;
    access, 68, 4;
    generation, 72, 8;
    incarnation, 80, 8;
    allocation_bytes, 88, 8;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::process::{Command, Stdio};

    #[cfg(target_os = "linux")]
    #[test]
    fn managed_checksum_matches_actual_sdk_provider_function() {
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../mem_service");
        let directory = std::env::temp_dir().join(format!(
            "managed-sdk-checksum-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&directory).unwrap();
        let source = directory.join("checksum.c");
        let binary = directory.join("checksum");
        std::fs::write(
            &source,
            r#"
#include "components/mem_service/mem_service_provider.h"
#include <stdio.h>
#include <inttypes.h>
int main(void) {
    unsigned char bytes[8192];
    size_t length = fread(bytes, 1, sizeof(bytes), stdin);
    if (ferror(stdin) || !feof(stdin)) return 2;
    printf("%" PRIu64 "\n", mem_service_provider_checksum64(bytes, length));
    return 0;
}
"#,
        )
        .unwrap();
        let build = Command::new("cc")
            .args([
                "-std=c11",
                "-D_GNU_SOURCE",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-ffunction-sections",
                "-fdata-sections",
                "-Wl,--gc-sections",
                "-I",
            ])
            .arg(&root)
            .arg(&source)
            .arg(root.join("components/mem_service/mem_service_provider.c"))
            .arg("-o")
            .arg(&binary)
            .output()
            .unwrap();
        assert!(
            build.status.success(),
            "{}",
            String::from_utf8_lossy(&build.stderr)
        );
        for bytes in [
            vec![],
            vec![0; 64],
            vec![0x5a; 64],
            (0..8191).map(|i| ((i * 73 + 19) % 256) as u8).collect(),
        ] {
            let mut child = Command::new(&binary)
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .spawn()
                .unwrap();
            child.stdin.take().unwrap().write_all(&bytes).unwrap();
            let output = child.wait_with_output().unwrap();
            assert!(output.status.success());
            assert_eq!(
                String::from_utf8(output.stdout)
                    .unwrap()
                    .trim()
                    .parse::<u64>()
                    .unwrap(),
                payload_checksum(&bytes)
            );
        }
        for path in [&binary, &source] {
            std::fs::remove_file(path).unwrap();
        }
        std::fs::remove_dir(directory).unwrap();
    }

    #[test]
    fn matches_locked_c_sdk_codec_and_rejections() {
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../mem_service");
        let directory = std::env::temp_dir().join(format!(
            "managed-ref-interop-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&directory).unwrap();
        let source = directory.join("codec.c");
        let binary = directory.join("codec");
        std::fs::write(
            &source,
            r#"
#include "components/mem_service/lingqu_object_service.h"
#include <stdio.h>
int main(int argc, char **argv) {
    unsigned char bytes[256];
    struct lingqu_object_ref_wire_v2 ref = {0};
    (void)argv;
    if (argc > 1) {
        if (fread(bytes, 1, sizeof(bytes), stdin) != sizeof(bytes)) return 2;
        return lingqu_object_ref_v2_decode(bytes, sizeof(bytes), &ref) ? 1 : 0;
    }
    ref.object.magic = LINGQU_OBJECT_REF_MAGIC;
    ref.object.layout_version = 2; ref.object.object_kind = 7;
    ref.object.state = 2; ref.object.object_version = 13;
    ref.object.owner_entity = 2; ref.object.producer_entity = 3;
    ref.object.payload_offset = 4224; ref.object.payload_bytes = 1024;
    ref.object.payload_checksum = UINT64_C(0xfedcba9876543210);
    ref.wire_bytes = sizeof(bytes); ref.access = 1;
    ref.allocation_generation = 17; ref.provider_incarnation = 29;
    ref.allocation_bytes = 8192;
    memcpy(ref.allocation_key, "w5.hidden", 9); memcpy(ref.home_node, "nodeC", 5);
    ref.object.key_hash = lingqu_object_ref_key_hash(ref.allocation_key, 9);
    if (lingqu_object_ref_v2_encode(&ref, bytes, sizeof(bytes))) return 3;
    return fwrite(bytes, 1, sizeof(bytes), stdout) == sizeof(bytes) ? 0 : 4;
}
"#,
        )
        .unwrap();
        let build = Command::new("cc")
            .args(["-std=c11", "-Wall", "-Wextra", "-Werror"])
            .arg("-I")
            .arg(root)
            .arg(&source)
            .arg("-o")
            .arg(&binary)
            .output()
            .unwrap();
        assert!(
            build.status.success(),
            "{}",
            String::from_utf8_lossy(&build.stderr)
        );
        let encoded = Command::new(&binary).output().unwrap();
        assert!(encoded.status.success());
        let reference = ManagedObjectRef::from_le_bytes(&encoded.stdout).unwrap();
        assert_eq!(reference.as_le_bytes().as_slice(), encoded.stdout);
        assert_eq!(reference.allocation_key(), b"w5.hidden");
        assert_eq!(reference.home_node(), b"nodeC");
        assert_eq!((reference.generation(), reference.incarnation()), (17, 29));
        assert_eq!(
            (reference.payload_offset(), reference.payload_bytes()),
            (4224, 1024)
        );
        assert_eq!(reference.payload_checksum(), 0xfedc_ba98_7654_3210);
        for length in [0, 64, 255, 257] {
            assert!(ManagedObjectRef::from_le_bytes(&vec![0; length]).is_err());
        }
        // Include valid identity changes as well as malformed wire cases.
        for (offset, value) in [
            (0, 0),
            (8, 1),
            (10, 0),
            (12, 1),
            (14, 1),
            (24, 0),
            (32, 0),
            (40, 255),
            (47, 255),
            (55, 255),
            (64, 0),
            (65, 0),
            (68, 0),
            (68, 4),
            (68, 3),
            (72, 0),
            (72, 18),
            (80, 0),
            (80, 30),
            (89, 0),
            (96, b'='),
            (96, b' '),
            (106, b'x'),
            (192, 0),
            (198, b'x'),
            (255, b'x'),
        ] {
            let mut bytes = encoded.stdout.clone();
            bytes[offset] = value;
            let mut child = Command::new(&binary)
                .arg("check")
                .stdin(Stdio::piped())
                .stdout(Stdio::null())
                .spawn()
                .unwrap();
            child.stdin.take().unwrap().write_all(&bytes).unwrap();
            let sdk_valid = child.wait().unwrap().success();
            let decoded = ManagedObjectRef::from_le_bytes(&bytes);
            assert_eq!(decoded.is_ok(), sdk_valid, "offset={offset} value={value}");
            if let Ok(changed) = decoded {
                if bytes != encoded.stdout {
                    assert_ne!(changed, reference);
                }
            }
        }
        std::fs::remove_file(source).unwrap();
        std::fs::remove_file(binary).unwrap();
        std::fs::remove_dir(directory).unwrap();
    }
}
