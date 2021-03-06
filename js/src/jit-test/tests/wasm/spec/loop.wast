;; Test `loop` opcode

(module
  (func $dummy)

  (func (export "empty")
    (loop)
    (loop $l)
  )

  (func (export "singular") (result i32)
    (loop (nop))
    (loop i32 (i32.const 7))
  )

  (func (export "multi") (result i32)
    (loop (call $dummy) (call $dummy) (call $dummy) (call $dummy))
    (loop i32 (call $dummy) (call $dummy) (call $dummy) (i32.const 8))
  )

  (func (export "nested") (result i32)
    (loop i32
      (loop (call $dummy) (block) (nop))
      (loop i32 (call $dummy) (i32.const 9))
    )
  )

  (func (export "deep") (result i32)
    (loop i32 (block i32 (loop i32 (block i32 (loop i32 (block i32
      (loop i32 (block i32 (loop i32 (block i32 (loop i32 (block i32
        (loop i32 (block i32 (loop i32 (block i32 (loop i32 (block i32
          (loop i32 (block i32 (loop i32 (block i32 (loop i32 (block i32
            (loop i32 (block i32 (loop i32 (block i32 (loop i32 (block i32
              (loop i32 (block i32 (loop i32 (block i32 (loop i32 (block i32
                (loop i32 (block i32 (loop i32 (block i32 (loop i32 (block i32
                  (loop i32 (block i32 (call $dummy) (i32.const 150)))
                ))))))
              ))))))
            ))))))
          ))))))
        ))))))
      ))))))
    ))))))
  )

  (func (export "as-unary-operand") (result i32)
    (i32.ctz (loop i32 (call $dummy) (i32.const 13)))
  )
  (func (export "as-binary-operand") (result i32)
    (i32.mul
      (loop i32 (call $dummy) (i32.const 3))
      (loop i32 (call $dummy) (i32.const 4))
    )
  )
  (func (export "as-test-operand") (result i32)
    (i32.eqz (loop i32 (call $dummy) (i32.const 13)))
  )
  (func (export "as-compare-operand") (result i32)
    (f32.gt
      (loop f32 (call $dummy) (f32.const 3))
      (loop f32 (call $dummy) (f32.const 3))
    )
  )

  (func (export "break-bare") (result i32)
    (block (loop (br 1) (br 0) (unreachable)))
    (block (loop (br_if 1 (i32.const 1)) (unreachable)))
    (block (loop (br_table 1 (i32.const 0)) (unreachable)))
    (block (loop (br_table 1 1 1 (i32.const 1)) (unreachable)))
    (i32.const 19)
  )
  (func (export "break-value") (result i32)
    (block i32 (loop i32 (br 1 (i32.const 18)) (br 0) (i32.const 19)))
  )
  (func (export "break-repeated") (result i32)
    (block i32
      (loop i32
        (br 1 (i32.const 18))
        (br 1 (i32.const 19))
        (br_if 1 (i32.const 20) (i32.const 0))
        (br_if 1 (i32.const 20) (i32.const 1))
        (br 1 (i32.const 21))
        (br_table 1 (i32.const 22) (i32.const 0))
        (br_table 1 1 1 (i32.const 23) (i32.const 1))
        (i32.const 21)
      )
    )
  )
  (func (export "break-inner") (result i32)
    (local i32)
    (set_local 0 (i32.const 0))
    (set_local 0 (i32.add (get_local 0) (block i32 (loop i32 (block i32 (br 2 (i32.const 0x1)))))))
    (set_local 0 (i32.add (get_local 0) (block i32 (loop i32 (loop i32 (br 2 (i32.const 0x2)))))))
    (set_local 0 (i32.add (get_local 0) (block i32 (loop i32 (block i32 (loop i32 (br 1 (i32.const 0x4))))))))
    (set_local 0 (i32.add (get_local 0) (block i32 (loop i32 (i32.ctz (br 1 (i32.const 0x8)))))))
    (set_local 0 (i32.add (get_local 0) (block i32 (loop i32 (i32.ctz (loop i32 (br 2 (i32.const 0x10))))))))
    (get_local 0)
  )
  (func (export "cont-inner") (result i32)
    (local i32)
    (set_local 0 (i32.const 0))
    (set_local 0 (i32.add (get_local 0) (loop i32 (loop i32 (br 1)))))
    (set_local 0 (i32.add (get_local 0) (loop i32 (i32.ctz (br 0)))))
    (set_local 0 (i32.add (get_local 0) (loop i32 (i32.ctz (loop i32 (br 1))))))
    (get_local 0)
  )

  (func $fx (export "effects") (result i32)
    (local i32)
    (block
      (loop
        (set_local 0 (i32.const 1))
        (set_local 0 (i32.mul (get_local 0) (i32.const 3)))
        (set_local 0 (i32.sub (get_local 0) (i32.const 5)))
        (set_local 0 (i32.mul (get_local 0) (i32.const 7)))
        (br 1)
        (set_local 0 (i32.mul (get_local 0) (i32.const 100)))
      )
    )
    (i32.eq (get_local 0) (i32.const -14))
  )

  (func (export "while") (param i64) (result i64)
    (local i64)
    (set_local 1 (i64.const 1))
    (block
      (loop
        (br_if 1 (i64.eqz (get_local 0)))
        (set_local 1 (i64.mul (get_local 0) (get_local 1)))
        (set_local 0 (i64.sub (get_local 0) (i64.const 1)))
        (br 0)
      )
    )
    (get_local 1)
  )

  (func (export "for") (param i64) (result i64)
    (local i64 i64)
    (set_local 1 (i64.const 1))
    (set_local 2 (i64.const 2))
    (block
      (loop
        (br_if 1 (i64.gt_u (get_local 2) (get_local 0)))
        (set_local 1 (i64.mul (get_local 1) (get_local 2)))
        (set_local 2 (i64.add (get_local 2) (i64.const 1)))
        (br 0)
      )
    )
    (get_local 1)
  )

  (func (export "nesting") (param f32 f32) (result f32)
    (local f32 f32)
    (block
      (loop
        (br_if 1 (f32.eq (get_local 0) (f32.const 0)))
        (set_local 2 (get_local 1))
        (block
          (loop
            (br_if 1 (f32.eq (get_local 2) (f32.const 0)))
            (br_if 3 (f32.lt (get_local 2) (f32.const 0)))
            (set_local 3 (f32.add (get_local 3) (get_local 2)))
            (set_local 2 (f32.sub (get_local 2) (f32.const 2)))
            (br 0)
          )
        )
        (set_local 3 (f32.div (get_local 3) (get_local 0)))
        (set_local 0 (f32.sub (get_local 0) (f32.const 1)))
        (br 0)
      )
    )
    (get_local 3)
  )
)

(assert_return (invoke "empty"))
(assert_return (invoke "singular") (i32.const 7))
(assert_return (invoke "multi") (i32.const 8))
(assert_return (invoke "nested") (i32.const 9))
(assert_return (invoke "deep") (i32.const 150))

(assert_return (invoke "as-unary-operand") (i32.const 0))
(assert_return (invoke "as-binary-operand") (i32.const 12))
(assert_return (invoke "as-test-operand") (i32.const 0))
(assert_return (invoke "as-compare-operand") (i32.const 0))

(assert_return (invoke "break-bare") (i32.const 19))
(assert_return (invoke "break-value") (i32.const 18))
(assert_return (invoke "break-repeated") (i32.const 18))
(assert_return (invoke "break-inner") (i32.const 0x1f))

(assert_return (invoke "effects") (i32.const 1))

(assert_return (invoke "while" (i64.const 0)) (i64.const 1))
(assert_return (invoke "while" (i64.const 1)) (i64.const 1))
(assert_return (invoke "while" (i64.const 2)) (i64.const 2))
(assert_return (invoke "while" (i64.const 3)) (i64.const 6))
(assert_return (invoke "while" (i64.const 5)) (i64.const 120))
(assert_return (invoke "while" (i64.const 20)) (i64.const 2432902008176640000))

(assert_return (invoke "for" (i64.const 0)) (i64.const 1))
(assert_return (invoke "for" (i64.const 1)) (i64.const 1))
(assert_return (invoke "for" (i64.const 2)) (i64.const 2))
(assert_return (invoke "for" (i64.const 3)) (i64.const 6))
(assert_return (invoke "for" (i64.const 5)) (i64.const 120))
(assert_return (invoke "for" (i64.const 20)) (i64.const 2432902008176640000))

(assert_return (invoke "nesting" (f32.const 0) (f32.const 7)) (f32.const 0))
(assert_return (invoke "nesting" (f32.const 7) (f32.const 0)) (f32.const 0))
(assert_return (invoke "nesting" (f32.const 1) (f32.const 1)) (f32.const 1))
(assert_return (invoke "nesting" (f32.const 1) (f32.const 2)) (f32.const 2))
(assert_return (invoke "nesting" (f32.const 1) (f32.const 3)) (f32.const 4))
(assert_return (invoke "nesting" (f32.const 1) (f32.const 4)) (f32.const 6))
(assert_return (invoke "nesting" (f32.const 1) (f32.const 100)) (f32.const 2550))
(assert_return (invoke "nesting" (f32.const 1) (f32.const 101)) (f32.const 2601))
(assert_return (invoke "nesting" (f32.const 2) (f32.const 1)) (f32.const 1))
(assert_return (invoke "nesting" (f32.const 3) (f32.const 1)) (f32.const 1))
(assert_return (invoke "nesting" (f32.const 10) (f32.const 1)) (f32.const 1))
(assert_return (invoke "nesting" (f32.const 2) (f32.const 2)) (f32.const 3))
(assert_return (invoke "nesting" (f32.const 2) (f32.const 3)) (f32.const 4))
(assert_return (invoke "nesting" (f32.const 7) (f32.const 4)) (f32.const 10.3095235825))
(assert_return (invoke "nesting" (f32.const 7) (f32.const 100)) (f32.const 4381.54785156))
(assert_return (invoke "nesting" (f32.const 7) (f32.const 101)) (f32.const 2601))

(assert_invalid
  (module (func $type-empty-i32 (result i32) (loop)))
  "type mismatch"
)
(assert_invalid
  (module (func $type-empty-i64 (result i64) (loop)))
  "type mismatch"
)
(assert_invalid
  (module (func $type-empty-f32 (result f32) (loop)))
  "type mismatch"
)
(assert_invalid
  (module (func $type-empty-f64 (result f64) (loop)))
  "type mismatch"
)

(assert_invalid
  (module (func $type-binary (result i64)
    (loop i64 i64 (i64.const 1) (i64.const 2)) (i64.add)
  ))
  "invalid result arity"
)
(assert_invalid
  (module (func $type-binary-with-nop (result i32)
    (loop i32 i32 (nop) (i32.const 7) (nop) (i32.const 8)) i32.add
  ))
  "invalid result arity"
)

(assert_invalid
  (module (func $type-value-void-vs-num (result i32)
    (loop (nop))
  ))
  "type mismatch"
)
(assert_invalid
  (module (func $type-value-num-vs-num (result i32)
    (loop (f32.const 0))
  ))
  "type mismatch"
)

(; TODO(stack): soft failure
(assert_invalid
  (module (func $type-value-void-vs-num-after-break (result i32)
    (loop (br 1 (i32.const 1)) (nop))
  ))
  "type mismatch"
)
(assert_invalid
  (module (func $type-value-num-vs-num-after-break (result i32)
    (loop (br 1 (i32.const 1)) (f32.const 0))
  ))
  "type mismatch"
)
;)

(assert_invalid
  (module (func $type-cont-last-void-vs-empty (result i32)
    (loop (br 0 (nop)))
  ))
  "type mismatch"
)
(assert_invalid
  (module (func $type-cont-last-num-vs-empty (result i32)
    (loop (br 0 (i32.const 0)))
  ))
  "type mismatch"
)

;; TODO(stack): move these elsewhere
(module (func $type-cont-num-vs-void
  (loop (i32.const 0) (br 0))
))
(module (func $type-cont-nested-num-vs-void
  (block (loop (i32.const 1) (loop (i32.const 1) (br 2)) (br 1)))
))
