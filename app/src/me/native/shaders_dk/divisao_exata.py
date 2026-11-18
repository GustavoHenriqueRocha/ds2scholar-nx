#!/usr/bin/env python3
"""Troca toda divisão e todo módulo inteiro de um GLSL por funções de divisão exata.

Por quê: o uam (codegen do nouveau, `NVC0LegalizeSSA::handleDIV` -> `emulateIDIVMOD`) faz a/b inteiro com
`trunc(float(a) * rcp(float(b)))`, sem passo de correção, e o MUFU.RCP do Maxwell não arredonda certo: com o RCP
1 ulp abaixo, 80/80 dá 0 (relatório 03 da tradução, risco 1; o próprio uam avisa "non-constant integer
division/modulo ... floating point emulation with resulting loss of precision"). As funções `ds2_udiv`/`ds2_umod`
fazem a mesma estimativa, refinam com o resto e corrigem ±1: exatas para a, b < 2^31.

A troca é textual, operador a operador: o operando da esquerda é a cadeia multiplicativa (* / %) que termina no
operador; o da direita, uma expressão unária/pós-fixa. Divisão de ponto flutuante fica como está; o tipo é deduzido
pelos literais e pelos nomes declarados float/vec no próprio arquivo. As duas redes de segurança ficam no build
(`gerar_dksh.py`): um operando float trocado por engano não compila (não há `ds2_udiv(float, float)`), e uma divisão
inteira que escapou aparece como UDIV/UMOD/IDIV/MOD no TGSI do uam, e o build falha.

Cópia idêntica em ds2scholar-nx app/src/me/native/shaders_dk/divisao_exata.py (o port compila sozinho); a canônica
é a de ds2-deko3d tools/shaders/ (testes em test_divisao_exata.py).
"""
import re

HELPERS = """\
// ds2-deko3d: exact integer division (uam divides through an f32 reciprocal with no correction step, so a/b can be
// one off: 80/80 = 0). Estimate, refine with the remainder, then fix +-1: exact for a, b < 2^31.
uint ds2_udiv(uint a, uint b) {
  float rb = 1.0 / float(b);
  uint q = uint(max(float(a) * rb, 0.0));
  int r = int(a - q * b);
  q = uint(int(q) + int(float(r) * rb));
  r = int(a - q * b);
  if (r < 0) q -= 1u;
  else if (uint(r) >= b) q += 1u;
  return q;
}
uint ds2_umod(uint a, uint b) { return a - ds2_udiv(a, b) * b; }
uvec2 ds2_udiv(uvec2 a, uvec2 b) { return uvec2(ds2_udiv(a.x, b.x), ds2_udiv(a.y, b.y)); }
uvec2 ds2_umod(uvec2 a, uvec2 b) { return a - ds2_udiv(a, b) * b; }
uvec2 ds2_udiv(uvec2 a, uint b) { return ds2_udiv(a, uvec2(b)); }
uvec2 ds2_umod(uvec2 a, uint b) { return ds2_umod(a, uvec2(b)); }
uvec3 ds2_udiv(uvec3 a, uvec3 b) { return uvec3(ds2_udiv(a.x, b.x), ds2_udiv(a.y, b.y), ds2_udiv(a.z, b.z)); }
uvec3 ds2_umod(uvec3 a, uvec3 b) { return a - ds2_udiv(a, b) * b; }
uvec4 ds2_udiv(uvec4 a, uvec4 b) { return uvec4(ds2_udiv(a.xy, b.xy), ds2_udiv(a.zw, b.zw)); }
uvec4 ds2_umod(uvec4 a, uvec4 b) { return a - ds2_udiv(a, b) * b; }
int ds2_udiv(int a, int b) {
  uint q = ds2_udiv(uint(abs(a)), uint(abs(b)));
  return ((a < 0) != (b < 0)) ? -int(q) : int(q);
}
int ds2_umod(int a, int b) { return a - ds2_udiv(a, b) * b; }
ivec2 ds2_udiv(ivec2 a, ivec2 b) { return ivec2(ds2_udiv(a.x, b.x), ds2_udiv(a.y, b.y)); }
ivec2 ds2_umod(ivec2 a, ivec2 b) { return a - ds2_udiv(a, b) * b; }
ivec2 ds2_udiv(ivec2 a, int b) { return ds2_udiv(a, ivec2(b)); }
ivec2 ds2_umod(ivec2 a, int b) { return ds2_umod(a, ivec2(b)); }
"""
MARCA = "uint ds2_udiv(uint a, uint b) {"

TOKEN = re.compile(r"""
    (?P<num>(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?[uUfF]?|0[xX][0-9a-fA-F]+[uU]?)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op><<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||\^\^|\+\+|--|\+=|-=|\*=|/=|%=|&=|\|=|\^=|[-+*/%<>=!~&|^?:;,.(){}\[\]])
  | (?P<sp>\s+)
""", re.X)
FLOAT_LITERAL = re.compile(r"^(?:\d+\.\d*|\.\d+|\d+[eE])")
INT_CTOR = {"uint", "int", "uvec2", "uvec3", "uvec4", "ivec2", "ivec3", "ivec4", "floatBitsToUint", "floatBitsToInt",
            "bitCount", "findLSB", "findMSB", "textureSize", "imageSize", "packHalf2x16", "packUnorm4x8"}
FLOAT_FUNC = {"float", "vec2", "vec3", "vec4", "texture", "textureLod", "length", "dot", "sqrt", "inversesqrt",
              "exp", "exp2", "log", "log2", "pow", "fract", "floor", "ceil", "uintBitsToFloat", "intBitsToFloat",
              "unpackHalf2x16", "unpackUnorm4x8", "mix", "smoothstep", "step", "dFdx", "dFdy", "fwidth"}
SWIZZLE = re.compile(r"^(?:[xyzw]{1,4}|[rgba]{1,4}|[stpq]{1,4})$")
# Lower than * / %: what ends the left operand going backwards (at depth 0).
PARA = {"+", "-", "<<", ">>", "<", ">", "<=", ">=", "==", "!=", "&", "^", "|", "&&", "||", "^^", "?", ":", "=", "+=",
        "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^=", ",", ";", "{", "}", "return"}


def _sem_comentarios(texto):
    """O texto com comentários e linhas de pré-processador trocados por espaço (mesmas posições)."""
    saida = list(texto)
    i = 0
    while i < len(texto):
        if texto.startswith("//", i) or (texto[i] == "#" and (i == 0 or texto[i - 1] == "\n")):
            fim = texto.find("\n", i)
            fim = len(texto) if fim < 0 else fim
        elif texto.startswith("/*", i):
            fim = texto.find("*/", i)
            fim = len(texto) if fim < 0 else fim + 2
        else:
            i += 1
            continue
        for j in range(i, fim):
            if saida[j] != "\n":
                saida[j] = " "
        i = fim
    return "".join(saida)


def _tokens(texto):
    out = []
    for m in TOKEN.finditer(texto):
        if m.lastgroup != "sp":
            out.append((m.group(), m.start(), m.end(), m.lastgroup))
    return out


def nomes_float(texto):
    """Variáveis, membros e funções declarados float/vec/mat no arquivo."""
    limpo = _sem_comentarios(texto)
    nomes = set()
    for m in re.finditer(r"\b(?:float|vec[234]|mat[234](?:x[234])?)\s+(\w+)(\s*(?:\[[^\]]*\])?\s*(?:,\s*\w+)*)", limpo):
        nomes.add(m.group(1))
        nomes.update(re.findall(r",\s*(\w+)", m.group(2)))
    return nomes


def _inicio_esquerda(tokens, k):
    """Índice do primeiro token do operando à esquerda do operador tokens[k]."""
    j = k - 1
    inicio = k
    while j >= 0:
        t = tokens[j][0]
        if t in (")", "]"):
            nivel, abre = 0, "(" if t == ")" else "["
            while j >= 0:
                if tokens[j][0] == t:
                    nivel += 1
                elif tokens[j][0] == abre:
                    nivel -= 1
                    if nivel == 0:
                        break
                j -= 1
            inicio = j
            j -= 1
            continue
        if t in ("(", "["):
            break
        if t in ("+", "-", "!", "~"):
            anterior = tokens[j - 1][0] if j > 0 else ";"
            unario = anterior in PARA or anterior in ("(", "[", "*", "/", "%", "!", "~")
            if not unario:
                break
            inicio = j
            j -= 1
            continue
        if t in PARA:
            break
        inicio = j
        j -= 1
    return inicio


def _fim_direita(tokens, k):
    """Índice do último token do operando à direita do operador tokens[k] (unário + pós-fixo)."""
    j = k + 1
    while j < len(tokens) and tokens[j][0] in ("+", "-", "!", "~"):
        j += 1
    if j >= len(tokens):
        raise ValueError("divisão sem operando à direita")
    if tokens[j][0] == "(":
        j = _fecha(tokens, j)
    elif tokens[j][3] in ("id", "num"):
        if j + 1 < len(tokens) and tokens[j + 1][0] == "(":
            j = _fecha(tokens, j + 1)
    else:
        raise ValueError(f"operando à direita inesperado: {tokens[j][0]}")
    while j + 1 < len(tokens):
        if tokens[j + 1][0] == "[":
            j = _fecha(tokens, j + 1, "[", "]")
        elif tokens[j + 1][0] == "." and j + 2 < len(tokens) and tokens[j + 2][3] == "id":
            j += 2
        elif tokens[j + 1][0] in ("++", "--"):
            j += 1
        else:
            break
    return j


def _fecha(tokens, j, abre="(", fecha=")"):
    nivel = 0
    for i in range(j, len(tokens)):
        if tokens[i][0] == abre:
            nivel += 1
        elif tokens[i][0] == fecha:
            nivel -= 1
            if nivel == 0:
                return i
    raise ValueError("parêntese sem par")


def _e_float(toks, floats):
    """Um operando é float se algum literal float ou nome float aparece fora de um construtor inteiro."""
    i = 0
    while i < len(toks):
        t, _, _, tipo = toks[i]
        if tipo == "id" and t in INT_CTOR and i + 1 < len(toks) and toks[i + 1][0] == "(":
            i = _fecha(toks, i + 1) + 1
            continue
        if tipo == "num" and FLOAT_LITERAL.match(t) and not t.lower().startswith("0x"):
            return True
        if tipo == "id":
            depois_ponto = i > 0 and toks[i - 1][0] == "."
            if depois_ponto and SWIZZLE.match(t):
                pass
            elif t in floats or (t in FLOAT_FUNC and i + 1 < len(toks) and toks[i + 1][0] == "("):
                return True
        i += 1
    return False


def trocar(texto):
    """(texto novo, número de trocas). Idempotente: um texto já trocado não muda."""
    floats = nomes_float(texto)
    trocas = 0
    ignorar = set()  # posições de divisões float já vistas
    while True:
        limpo = _sem_comentarios(texto)
        tokens = _tokens(limpo)
        alvo = None
        for k, (t, ini, _, _) in enumerate(tokens):
            if t in ("/", "%") and ini not in ignorar:
                i0, i1 = _inicio_esquerda(tokens, k), _fim_direita(tokens, k)
                esquerda, direita = tokens[i0:k], tokens[k + 1:i1 + 1]
                if not esquerda:
                    raise ValueError(f"divisão sem operando à esquerda na posição {ini}")
                if t == "/" and (_e_float(esquerda, floats) or _e_float(direita, floats)):
                    ignorar.add(ini)
                    continue
                alvo = (k, i0, i1)
                break
        if alvo is None:
            break
        k, i0, i1 = alvo
        op = tokens[k][0]
        a = texto[tokens[i0][1]:tokens[k][1]].rstrip()
        b = texto[tokens[k][2]:tokens[i1][2]].strip()
        nome = "ds2_udiv" if op == "/" else "ds2_umod"
        novo = f"{nome}({a}, {b})"
        inicio, fim = tokens[i0][1], tokens[i1][2]
        delta = len(novo) - (fim - inicio)
        texto = texto[:inicio] + novo + texto[fim:]
        ignorar = {p + delta if p > inicio else p for p in ignorar}
        trocas += 1
    if trocas and MARCA not in texto:
        texto = inserir_helpers(texto)
    return texto, trocas


def inserir_helpers(texto):
    """As funções vão antes da primeira função do arquivo (depois das declarações globais)."""
    limpo = _sem_comentarios(texto)
    m = re.search(r"^[A-Za-z_]\w*\s+[A-Za-z_]\w*\s*\([^;{]*\)\s*\{", limpo, re.M)
    pos = m.start() if m else len(texto)
    return texto[:pos] + HELPERS + "\n" + texto[pos:]
