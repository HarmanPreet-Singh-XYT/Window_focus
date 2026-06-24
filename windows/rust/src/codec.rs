/// Flutter StandardMethodCodec — pure Rust encode/decode.
///
/// Wire format reference:
///   Method call: [0x07, <size_prefix(method_name)>, <method_name_utf8>, <encoded_args>]
///   Success response: [0x00, <encoded_value>]
///   Error response:   [0x01, <encoded_string(code)>, <encoded_string(message)>, 0x00]
///   Not implemented:  [] (empty)

// ---- Value type ----

#[derive(Debug, Clone)]
pub enum EncodableValue {
    Null,
    Bool(bool),
    Int32(i32),
    Int64(i64),
    Float64(f64),
    String(String),
    Bytes(Vec<u8>),
    Map(Vec<(EncodableValue, EncodableValue)>),
}

// Type tags
const TAG_NULL: u8 = 0x00;
const TAG_TRUE: u8 = 0x01;
const TAG_FALSE: u8 = 0x02;
const TAG_INT32: u8 = 0x03;
const TAG_INT64: u8 = 0x04;
const TAG_FLOAT64: u8 = 0x06;
const TAG_STRING: u8 = 0x07;
const TAG_UINT8_LIST: u8 = 0x08;
const TAG_MAP: u8 = 0x0C;

// ---- Size prefix encoding ----

fn encode_size(buf: &mut Vec<u8>, size: usize) {
    if size <= 253 {
        buf.push(size as u8);
    } else if size <= 0xFFFF {
        buf.push(0xFE);
        buf.push((size & 0xFF) as u8);
        buf.push((size >> 8) as u8);
    } else {
        buf.push(0xFF);
        buf.push((size & 0xFF) as u8);
        buf.push((size >> 8 & 0xFF) as u8);
        buf.push((size >> 16 & 0xFF) as u8);
        buf.push((size >> 24 & 0xFF) as u8);
    }
}

fn decode_size(bytes: &[u8], pos: &mut usize) -> Option<usize> {
    let b = *bytes.get(*pos)?;
    *pos += 1;
    match b {
        0xFF => {
            if *pos + 4 > bytes.len() {
                return None;
            }
            let v = u32::from_le_bytes(bytes[*pos..*pos + 4].try_into().ok()?);
            *pos += 4;
            Some(v as usize)
        }
        0xFE => {
            if *pos + 2 > bytes.len() {
                return None;
            }
            let v = u16::from_le_bytes(bytes[*pos..*pos + 2].try_into().ok()?);
            *pos += 2;
            Some(v as usize)
        }
        n => Some(n as usize),
    }
}

// ---- Encode a value into buf (buf is the complete output buffer for alignment calc) ----

pub fn encode_value(buf: &mut Vec<u8>, value: &EncodableValue) {
    match value {
        EncodableValue::Null => buf.push(TAG_NULL),
        EncodableValue::Bool(true) => buf.push(TAG_TRUE),
        EncodableValue::Bool(false) => buf.push(TAG_FALSE),
        EncodableValue::Int32(v) => {
            buf.push(TAG_INT32);
            buf.extend_from_slice(&v.to_le_bytes());
        }
        EncodableValue::Int64(v) => {
            buf.push(TAG_INT64);
            buf.extend_from_slice(&v.to_le_bytes());
        }
        EncodableValue::Float64(v) => {
            buf.push(TAG_FLOAT64);
            // Pad to 8-byte alignment from the start of the buffer
            let current_pos = buf.len();
            let aligned = (current_pos + 7) & !7;
            let padding = aligned - current_pos;
            for _ in 0..padding {
                buf.push(0);
            }
            buf.extend_from_slice(&v.to_bits().to_le_bytes());
        }
        EncodableValue::String(s) => {
            buf.push(TAG_STRING);
            encode_size(buf, s.len());
            buf.extend_from_slice(s.as_bytes());
        }
        EncodableValue::Bytes(b) => {
            buf.push(TAG_UINT8_LIST);
            encode_size(buf, b.len());
            buf.extend_from_slice(b);
        }
        EncodableValue::Map(pairs) => {
            buf.push(TAG_MAP);
            encode_size(buf, pairs.len());
            for (k, v) in pairs {
                encode_value(buf, k);
                encode_value(buf, v);
            }
        }
    }
}

// ---- Decode a value ----

pub fn decode_value(bytes: &[u8], pos: &mut usize) -> Option<EncodableValue> {
    let tag = *bytes.get(*pos)?;
    *pos += 1;
    match tag {
        TAG_NULL => Some(EncodableValue::Null),
        TAG_TRUE => Some(EncodableValue::Bool(true)),
        TAG_FALSE => Some(EncodableValue::Bool(false)),
        TAG_INT32 => {
            if *pos + 4 > bytes.len() {
                return None;
            }
            let v = i32::from_le_bytes(bytes[*pos..*pos + 4].try_into().ok()?);
            *pos += 4;
            Some(EncodableValue::Int32(v))
        }
        TAG_INT64 => {
            if *pos + 8 > bytes.len() {
                return None;
            }
            let v = i64::from_le_bytes(bytes[*pos..*pos + 8].try_into().ok()?);
            *pos += 8;
            Some(EncodableValue::Int64(v))
        }
        TAG_FLOAT64 => {
            // Skip alignment padding (8-byte from start of buffer, i.e. pos aligned to 8)
            let aligned = (*pos + 7) & !7;
            *pos = aligned;
            if *pos + 8 > bytes.len() {
                return None;
            }
            let bits = u64::from_le_bytes(bytes[*pos..*pos + 8].try_into().ok()?);
            *pos += 8;
            Some(EncodableValue::Float64(f64::from_bits(bits)))
        }
        TAG_STRING => {
            let len = decode_size(bytes, pos)?;
            if *pos + len > bytes.len() {
                return None;
            }
            let s = std::str::from_utf8(&bytes[*pos..*pos + len]).ok()?.to_owned();
            *pos += len;
            Some(EncodableValue::String(s))
        }
        TAG_UINT8_LIST => {
            let len = decode_size(bytes, pos)?;
            if *pos + len > bytes.len() {
                return None;
            }
            let b = bytes[*pos..*pos + len].to_vec();
            *pos += len;
            Some(EncodableValue::Bytes(b))
        }
        TAG_MAP => {
            let count = decode_size(bytes, pos)?;
            let mut pairs = Vec::with_capacity(count);
            for _ in 0..count {
                let k = decode_value(bytes, pos)?;
                let v = decode_value(bytes, pos)?;
                pairs.push((k, v));
            }
            Some(EncodableValue::Map(pairs))
        }
        // Typed lists — decode but return as Bytes for simplicity (our plugin doesn't use them)
        0x09 => {
            // int32 list
            let count = decode_size(bytes, pos)?;
            let byte_len = count * 4;
            if *pos + byte_len > bytes.len() {
                return None;
            }
            *pos += byte_len;
            Some(EncodableValue::Null)
        }
        0x0A => {
            // int64 list
            let count = decode_size(bytes, pos)?;
            let byte_len = count * 8;
            if *pos + byte_len > bytes.len() {
                return None;
            }
            *pos += byte_len;
            Some(EncodableValue::Null)
        }
        0x0B => {
            // float64 list
            let count = decode_size(bytes, pos)?;
            let byte_len = count * 8;
            if *pos + byte_len > bytes.len() {
                return None;
            }
            *pos += byte_len;
            Some(EncodableValue::Null)
        }
        0x0D => {
            // float32 list
            let count = decode_size(bytes, pos)?;
            let byte_len = count * 4;
            if *pos + byte_len > bytes.len() {
                return None;
            }
            *pos += byte_len;
            Some(EncodableValue::Null)
        }
        _ => None, // Unknown tag
    }
}

// ---- Public API ----

/// Decode an incoming method call from Dart.
/// Returns (method_name, args) or None if malformed.
pub fn decode_method_call(bytes: &[u8]) -> Option<(String, Option<EncodableValue>)> {
    let mut pos = 0;

    // First byte must be 0x07 (string tag) for method name
    let tag = *bytes.get(pos)?;
    if tag != TAG_STRING {
        return None;
    }
    pos += 1;

    let name_len = decode_size(bytes, &mut pos)?;
    if pos + name_len > bytes.len() {
        return None;
    }
    let method_name = std::str::from_utf8(&bytes[pos..pos + name_len])
        .ok()?
        .to_owned();
    pos += name_len;

    // Args may or may not be present
    let args = if pos < bytes.len() {
        decode_value(bytes, &mut pos)
    } else {
        None
    };

    Some((method_name, args))
}

/// Encode a success response envelope: [0x00, <encoded_value>]
pub fn encode_success(value: &EncodableValue) -> Vec<u8> {
    let mut buf = vec![0x00u8];
    encode_value(&mut buf, value);
    buf
}

/// Encode an error response envelope: [0x01, <code_string>, <message_string>, null]
pub fn encode_error(code: &str, message: &str) -> Vec<u8> {
    let mut buf = vec![0x01u8];
    encode_value(&mut buf, &EncodableValue::String(code.to_owned()));
    encode_value(&mut buf, &EncodableValue::String(message.to_owned()));
    encode_value(&mut buf, &EncodableValue::Null);
    buf
}

/// Encode a "not implemented" response (empty bytes).
pub fn encode_not_implemented() -> Vec<u8> {
    vec![]
}

/// Encode a method call envelope for invoking Dart methods from Rust:
/// [0x07, <size_prefix(method)>, <method_utf8>, <encoded_args>]
pub fn encode_method_call(method: &str, args: &EncodableValue) -> Vec<u8> {
    let mut buf = Vec::new();
    // Method name as string (tag + size + bytes)
    buf.push(TAG_STRING);
    encode_size(&mut buf, method.len());
    buf.extend_from_slice(method.as_bytes());
    // Arguments
    encode_value(&mut buf, args);
    buf
}

// ---- Map helpers ----

impl EncodableValue {
    /// Look up a key in a Map value by string key name.
    pub fn map_get(&self, key: &str) -> Option<&EncodableValue> {
        if let EncodableValue::Map(pairs) = self {
            for (k, v) in pairs {
                if let EncodableValue::String(s) = k {
                    if s == key {
                        return Some(v);
                    }
                }
            }
        }
        None
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            EncodableValue::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn as_i32(&self) -> Option<i32> {
        match self {
            EncodableValue::Int32(v) => Some(*v),
            EncodableValue::Int64(v) => Some(*v as i32),
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match self {
            EncodableValue::Float64(v) => Some(*v),
            EncodableValue::Int32(v) => Some(*v as f64),
            EncodableValue::Int64(v) => Some(*v as f64),
            _ => None,
        }
    }

}
