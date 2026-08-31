// llm/blob.S — 小型 LLM（Karpathy stories260K, llama2.c legacy 形式）と
// トークナイザをカーネル像に焼き込む。パスは compile/ からの相対。
.section .rodata
.balign 8
.global model_data
model_data:
.incbin "../llm/stories260K.bin"
.global model_data_end
model_data_end:

.balign 8
.global tok_data
tok_data:
.incbin "../llm/tok512.bin"
.global tok_data_end
tok_data_end:
