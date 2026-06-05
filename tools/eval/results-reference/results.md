# DIS-1018 — batbox-vs-Claude same-model orchestration eval (results)

Model held constant: `qwen2.5:7b` (temperature 0, via counting ollama proxy). Token accounting identical for both arms: sum of `usage.total_tokens` over all model calls, read from the proxy's `/__stats`.

| Task | Class | Arm | Outcome | Tokens | Model calls | Token Δ vs Claude |
|------|-------|-----|---------|--------|-------------|-------------------|
| lang-choice-2q | investigation | batbox | PASS | 2755 | 3 | +226.8% |
| lang-choice-2q | investigation | claude | PASS | 843 | 2 | — |
| lang-choice-4q-deep | investigation | batbox | PASS | 5642 | 5 | +238.9% |
| lang-choice-4q-deep | investigation | claude | PASS | 1665 | 4 | — |
| config-port-lookup | lookup | batbox | PASS | 1507 | 1 | +25.5% |
| config-port-lookup | lookup | claude | PASS | 1201 | 1 | — |

## Per-arm mechanism notes

**lang-choice-2q [investigation]**
- batbox: per-turn tokens (incl. spawn turn in q1): q1+=1715 q2+=1040 | a1[:55]="Summary: The research indicates that the choice of back"
- claude: per-turn tokens: q1=432 q2=411 | a1[:55]="Rust wins decisively for raw runtime performance. It of"

**lang-choice-4q-deep [investigation]**
- batbox: per-turn tokens (incl. spawn turn in q1): q1+=1800 q2+=1137 q3+=1291 q4+=1414 | a1[:55]="Based on the research, **Rust** would be the best fit f"
- claude: per-turn tokens: q1=432 q2=411 q3=395 q4=427 | a1[:55]="Rust wins decisively for raw runtime performance. It of"

**config-port-lookup [lookup]**
- batbox: stayed_closed=yes standing_count=0 | answer[:80]="listen_port = 8080"
- claude: answer[:80]="The service listens on port 8080."

