#!/usr/bin/env python3
"""Build a self-contained comparison from visible answers and a blind review."""
import argparse
import json
from pathlib import Path


PAGE = r'''<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Batbox creativity comparison</title>
<style>
:root{color-scheme:light dark;font:16px/1.55 system-ui,sans-serif;background:#101720;color:#e9eef5}
body{max-width:1350px;margin:auto;padding:24px}h1{font-size:1.9rem;margin-bottom:8px}p{max-width:85ch}
.muted{color:#acbacc}.grid{display:grid;grid-template-columns:1fr 1fr;gap:20px}.card,details{background:#182331;border:1px solid #344254;border-radius:12px;padding:20px;margin:18px 0}
select,button{font:inherit;padding:10px;border:1px solid #61748b;border-radius:6px;background:#101720;color:inherit}select{width:100%}button{cursor:pointer}
pre{white-space:pre-wrap;overflow-wrap:anywhere;font:inherit;margin:16px 0}.scores{display:grid;grid-template-columns:1fr 1fr;gap:6px;font-size:.9rem}.score{background:#243449;padding:8px;border-radius:6px}.meta{color:#9ecbff;font-size:.9rem;margin:12px 0}.review{font-size:.9rem}summary{cursor:pointer;font-weight:600}label{display:block;font-weight:600;margin-bottom:8px}.toolbar{display:flex;gap:12px;align-items:center;flex-wrap:wrap}a{color:#9ecbff}
@media(max-width:760px){.grid{grid-template-columns:1fr}body{padding:14px}.card{margin:6px 0}}
</style>
<h1>Batbox creativity comparison</h1>
<p>Compare the actual visible answers to the same creative challenge. Output tokens include reasoning and the visible answer; the reasoning text is excluded from this report.</p>
<p class="muted">Scores are a qualitative blind review on a 0–4 scale. They measure originality within these answers, usefulness, a concrete mechanism, and testability. They are not an intelligence score or a claim of worldwide novelty.</p>
<details><summary>The shared challenge</summary><pre id="challenge"></pre></details>
<div class="toolbar"><button id="hide">Hide setting labels</button><span class="muted" id="count"></span></div>
<div class="grid">
<article class="card"><label for="left">First answer</label><select id="left"></select><div id="left-meta" class="meta"></div><div id="left-scores" class="scores"></div><pre id="left-text"></pre><details class="review"><summary>Review evidence</summary><div id="left-review"></div></details></article>
<article class="card"><label for="right">Second answer</label><select id="right"></select><div id="right-meta" class="meta"></div><div id="right-scores" class="scores"></div><pre id="right-text"></pre><details class="review"><summary>Review evidence</summary><div id="right-review"></div></details></article>
</div>
<p class="muted">Correctness, task completion, and token-cost measurements are recorded separately in <a href="sampling-summary.json">sampling-summary.json</a>. These answers use the common sampling controls documented in <a href="creativity-results.json">creativity-results.json</a>.</p>
<script id="report" type="application/json">__DATA__</script>
<script>
'use strict';
const data=JSON.parse(document.getElementById('report').textContent);
const samples=data.samples.slice().sort((a,b)=>a.temperature-b.temperature || a.seed-b.seed);
const reviews=new Map(data.reviews.map(r=>[r.label,r]));
const criteria=[['originality','Originality'],['practical_usefulness','Usefulness'],['concrete_mechanism','Mechanism'],['testability','Testability']];
let hidden=false;
document.getElementById('challenge').textContent=data.challenge;
document.getElementById('count').textContent=samples.length+' visible answers · '+new Set(samples.map(s=>s.temperature)).size+' temperatures';
function render(side){
  const s=samples[Number(document.getElementById(side).value)];if(!s)return;
  const r=reviews.get(s.label);
  document.getElementById(side+'-text').textContent=s.text;
  const tokens=typeof s.total_output_tokens==='number'?s.total_output_tokens.toLocaleString():'unknown';
  document.getElementById(side+'-meta').textContent=(hidden?s.label:'Temperature '+s.temperature+' · seed '+s.seed+' · repetition penalty '+s.repetition_penalty)+' · '+tokens+' output tokens · '+(s.completed_before_output_cap?'completed before output cap':'reached output cap or did not finish');
  const scores=document.getElementById(side+'-scores');scores.replaceChildren();
  const evidence=document.getElementById(side+'-review');evidence.replaceChildren();
  for(const [key,title] of criteria){
    const box=document.createElement('div');box.className='score';box.textContent=title+': '+(r?.scores?.[key]??'unrated')+(r?'/4':'');scores.append(box);
    if(r?.evidence?.[key]){const p=document.createElement('p');p.textContent=title+': '+r.evidence[key];evidence.append(p);}
  }
  if(r?.shortcomings?.length){const p=document.createElement('p');p.textContent='Limitations: '+r.shortcomings.join(' ');evidence.append(p);}
}
function options(){for(const side of ['left','right']){const el=document.getElementById(side),old=el.value;el.replaceChildren();samples.forEach((s,i)=>{const o=document.createElement('option');o.value=i;o.textContent=hidden?s.label:'Temperature '+s.temperature+' · seed '+s.seed;el.append(o);});if(old)el.value=old;render(side);}}
options();
for(const side of ['left','right'])document.getElementById(side).addEventListener('change',()=>render(side));
const left=samples.findIndex(s=>s.temperature===data.left_temperature),right=samples.findIndex(s=>s.temperature===data.right_temperature);
document.getElementById('left').value=Math.max(0,left);document.getElementById('right').value=right<0?Math.max(0,samples.length-1):right;render('left');render('right');
document.getElementById('hide').addEventListener('click',()=>{hidden=!hidden;document.getElementById('hide').textContent=hidden?'Show setting labels':'Hide setting labels';options();});
</script></html>'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--answers', type=Path, required=True)
    parser.add_argument('--review', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--left-temperature', type=float, default=1.0)
    parser.add_argument('--right-temperature', type=float, default=1.2)
    args = parser.parse_args()
    answers = json.loads(args.answers.read_text())
    review = json.loads(args.review.read_text())
    sample_keys = ('label', 'temperature', 'seed', 'repetition_penalty',
                   'total_output_tokens', 'completed_before_output_cap', 'text')
    review_keys = ('label', 'scores', 'evidence', 'shortcomings')
    safe = {'challenge': answers['challenge'],
            'samples': [{k: s[k] for k in sample_keys} for s in answers['samples']],
            'reviews': [{k: r[k] for k in review_keys if k in r} for r in review['reviews']],
            'left_temperature': args.left_temperature, 'right_temperature': args.right_temperature}
    # Keep generated content inert even if an answer contains HTML/script text.
    data = json.dumps(safe, ensure_ascii=True).replace('<', '\\u003c').replace('>', '\\u003e').replace('&', '\\u0026')
    args.output.write_text(PAGE.replace('__DATA__', data))
    print(json.dumps({'output': str(args.output), 'visible_answers': len(safe['samples']),
                      'reviewed_answers': len(safe['reviews']), 'reasoning_exported': False}))


if __name__ == '__main__':
    main()
