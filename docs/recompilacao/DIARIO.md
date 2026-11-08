# Diário da recompilação

Regra: **uma entrada a cada passo concluído**, a mais nova no topo. Cada entrada diz o que foi feito, o que foi medido, o resultado e o
próximo passo. Erros vão para `ERROS.md` (aqui só o número). Plano: `docs/superpowers/plans/2026-11-08-recompilacao-nativa.md`.

## 2026-11-08 (noite) — macOS publicado
- GitHub Actions gerou os três `.dmg` (universal, Intel, Apple Silicon) e a suíte de 382 testes passou nos Macs Intel e Apple Silicon. Os arquivos
  foram anexados à Release v0.2.0, e a página e o README foram atualizados (commit `6924f28`). Detalhes em E01 e E05 (`ERROS.md`).
- **Próximo passo (inalterado):** F0 do plano.

## 2026-11-08 — Plano criado; ponto de partida medido
- **Decisão do usuário:** o objetivo é o *jogo* nativo de PC; o interpretador não conta como "100% nativo". Hoje o projeto é um
  emulador dedicado, e a descrição pública (README, página, Release v0.1.0, repositório) foi corrigida para dizer isso.
- **Linha de base (v0.2.0):** 382 testes passando; gameplay do estágio 1 a 8,8 ms por quadro (emulação 4,6 + vídeo 4,3), 0 quadros acima de
  16,7 ms. Ganho do interpretador (caminhos rápidos de memória + aceleração exata de laços de espera): de 15,5 para 8,8 ms, saída idêntica bit a bit.
- **Cobertura dinâmica (F0, primeira medição):** 61 912 endereços de instrução distintos executados em 9 600 quadros (boot até o gameplay do
  estágio 1), cerca de 5,5% das 1 119 640 instruções achadas estaticamente nos 79 programas. A cobertura de todas as fases ainda não foi feita.
- Achados novos durante a medição, ainda sem causa: ver E02 e E03 em `ERROS.md`.
- Outras entregas do dia: correção do estouro na interpolação do SCSP (E04); validação dos índices nos save states; Releases v0.1.0 e
  v0.2.0 (Linux e Windows); página do projeto; macOS preparado, aguardando o GitHub Actions (E01, E05).
- **Próximo passo:** F0 completa (cobertura por fase, alvos dos saltos indiretos, auto-modificação). Depois F1 (IR e lifter do SH-2).
