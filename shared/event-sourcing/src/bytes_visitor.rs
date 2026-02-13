pub struct BytesVisitor<const N: usize>;

impl<'de, const N: usize> serde::de::Visitor<'de> for BytesVisitor<N> {
    type Value = [u8; N];

    fn expecting(&self, formatter: &mut std::fmt::Formatter) -> std::fmt::Result {
        formatter.write_str(&format!("a {N}-byte array"))
    }

    fn visit_bytes<E>(self, v: &[u8]) -> Result<Self::Value, E>
    where
        E: serde::de::Error,
    {
        if v.len() != N {
            return Err(E::custom(format!("expected {N} bytes, got {}", v.len())));
        }
        let mut arr = [0u8; N];
        arr.copy_from_slice(v);
        Ok(arr)
    }

    fn visit_seq<A>(self, mut seq: A) -> Result<Self::Value, A::Error>
    where
        A: serde::de::SeqAccess<'de>,
    {
        let mut arr = [0u8; N];
        for (i, byte) in arr.iter_mut().enumerate() {
            *byte = seq
                .next_element()?
                .ok_or_else(|| serde::de::Error::invalid_length(i, &self))?;
        }
        Ok(arr)
    }
}
