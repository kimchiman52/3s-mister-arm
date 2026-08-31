import copy, json, cg_audit as A

def totals(res):
    t = {}
    for n in A.NAMES:
        for k, v in res[n]['stats'].items():
            if isinstance(v, int): t[k] = t.get(k, 0) + v
    per_a = {n: res[n]['stats']['a_oob'] for n in A.NAMES if res[n]['stats']['a_oob']}
    per_c = {n: res[n]['stats']['c_wrong_group'] + res[n]['stats']['c_same_group']
             for n in A.NAMES if res[n]['stats']['c_wrong_group'] + res[n]['stats']['c_same_group']}
    return t, per_a, per_c

def drop(pred):
    m = copy.deepcopy(A.CGMAP)
    for i, e in enumerate(m):
        e['ranges'] = [r for r in e['ranges'] if not pred(i, r)]
    return m

BASE = A.audit()
tb, ab, cb = totals(BASE)
print("BASELINE (HEAD cg_maps): class(a)=%d  class(c)=%d" % (tb['a_oob'], tb['c_wrong_group']+tb['c_same_group']))
print("   class(a) by char:", ab)

CASES = [
 ("PRE-#290  (drop elena 0x9C88-0x9CC1)",
  lambda i, r: i == 8 and r[0] == 0x9C88),
 ("PRE-#359  (drop sean 0x70F4-0x70FF)",
  lambda i, r: i == 12 and r[0] == 0x70F4),
 ("PRE-#360  (drop every 0x7070-0x714B transition range)",
  lambda i, r: 0x7070 <= r[0] <= 0x714B),
 ("PRE-#290+#359+#360 (all three fixes reverted)",
  lambda i, r: (i == 8 and r[0] == 0x9C88) or (i == 12 and r[0] == 0x70F4) or (0x7070 <= r[0] <= 0x714B)),
]
out = {"baseline": {"class_a_total": tb['a_oob'], "class_a_by_char": ab,
                    "class_c_total": tb['c_wrong_group']+tb['c_same_group']}}
for label, pred in CASES:
    r = A.audit(cgmap_override=drop(pred))
    t, a, c = totals(r)
    print("\n%s" % label)
    print("   class(a) OOB total = %d   (baseline %d)" % (t['a_oob'], tb['a_oob']))
    print("   class(a) by char   = %s" % a)
    print("   class(c) total     = %d   (baseline %d)" % (t['c_wrong_group']+t['c_same_group'],
                                                          tb['c_wrong_group']+tb['c_same_group']))
    out[label] = {"class_a_total": t['a_oob'], "class_a_by_char": a,
                  "class_c_total": t['c_wrong_group']+t['c_same_group']}
json.dump(out, open("cg_counterfactual.json", "w"), indent=1)
