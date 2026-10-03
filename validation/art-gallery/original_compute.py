#!/usr/bin/env python3
"""Real computed gallery cases. Inputs are numerical demonstrations, not calibrations.
Acceptance fixed before first run: <=12000 hexes; successful job within480s;
finite complete result arrays; equilibrium/heat diagnostics<=1e-6, thermal
enthalpy mismatch<=1e-9. Geometry classification error is reported, not hidden.
"""
import argparse,array,hashlib,json,math,struct,sys,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('kind',choices=['fdm','lpbf','thermal']);p.add_argument('--repo',type=Path,default=Path.cwd());p.add_argument('--out',type=Path,default=Path(__file__).parent);a=p.parse_args()
repo=a.repo.resolve();out=a.out.resolve();sys.path.insert(0,str(repo/'tools'))
from mcptest import Client
kind=a.kind; ws=out/'workspace';ws.mkdir(parents=True,exist_ok=True)
model={'fdm':'helical_flute_vase_mm','lpbf':'helical_window_column_mm','thermal':'thermal_corolla_mm'}[kind]
name={'fdm':'Helical_Flute','lpbf':'Helical_Lantern','thermal':'Thermal_Corolla'}[kind]
c=Client(['--embedded','--workspace',str(ws),'--allow-read',str(out)])
record={'kind':kind,'title':name.replace('_',' '),'scope':'Numerical demonstration with inferred inputs; no measurement comparison','geometry':model+'.stl','mesh_spacing_mm':.75,'acceptance':{'max_elements':12000,'residual_or_heat_balance':1e-6,'thermal_enthalpy_mismatch':1e-9,'finite_arrays':True,'runtime_budget_seconds':480}}
def call(op,args):
 r=c.call(op,args).get('result',{}).get('structuredContent',{})
 if not r.get('ok'): raise RuntimeError(op+': '+json.dumps(r))
 return r['value']
def save(): (out/(kind+'-run.json')).write_text(json.dumps(record,indent=2)+'\n')
try:
 c.initialize()
 project=call('project_open',{'path':str(ws/name)}) if (ws/name).exists() else call('project_create',{'name':name,'description':record['scope']})
 record['project']=project
 record['import']=call('geometry_import',{'path':str(out/(model+'.stl')),'units':'mm','name':'sculpture','replace':True})
 if kind=='thermal':
  call('material_define',{'material':{'id':'corolla_demo','name':'Thermal corolla demonstration','family':'metal','status':'demonstration','provenance':'Assumed constant properties for a numerical gallery demonstration','conductivity_w_per_mk':{'value':10},'density_kg_m3':{'value':7800},'specific_heat_j_per_kgk':{'value':500}}})
 mat='pla_generic_demo' if kind=='fdm' else 'ss316l_lpbf_demo' if kind=='lpbf' else 'corolla_demo'
 call('material_assign',{'body':'sculpture','material':mat,'source':'inferred'})
 record['mesh']=call('mesh_generate',{'element_size':'.75 mm','max_elements':12000})
 print(kind,'mesh:',json.dumps(record['mesh'].get('mesh',{})),flush=True)
 assert 0 < record['mesh']['mesh']['elements'] <=12000
 record['inspection']=call('mesh_inspect',{})
 if kind=='fdm':
  op='mech_print_run';args={'body':'sculpture','label':'Helical Flute: FDM numerical demonstration','process':{'layer_height':'1.5 mm','printed_layer_height':'.2 mm','nozzle_temperature':'210 degC','bed_temperature':'60 degC','ambient_temperature':'25 degC','deposition_rate':'8 mm^3/s','min_layer_time':'8 s','cooldown_bed_on':'120 s','cooldown_bed_off':'240 s','thermal_substeps':4,'provenance':'inferred'}}
 elif kind=='lpbf':
  op='lpbf_build_run';args={'body':'sculpture','label':'Helical Lantern: inherent-strain numerical demonstration','build_orientation':'X','layer_thickness_sim':'1.5 mm','inherent_strain':{'exx':-.0004,'eyy':-.0006,'ezz':-.002,'provenance':'inferred','source':'Demonstration eigenstrain tensor; not a calibrated machine or material'},'material':{'youngs_modulus':'215000 MPa','poissons_ratio':.3,'provenance':'inferred'},'cut':{'height':'1.5 mm','kerf':'.75 mm','from_x':'0 mm','provenance':'assumed'}}
 else:
  call('selection_create',{'name':'bottom','body':'sculpture','query':{'plane':{'axis':'z','at':'min'}},'source':'default'})
  call('selection_create',{'name':'outer','body':'sculpture','query':{'facing':{'direction':'up','max_angle_deg':180}},'source':'default'})
  call('boundary_apply',{'name':'bottom_pulse','kind':'heat_flux','selection':'bottom','heat_flux':'60000 W/m^2','schedule':[{'time':'0 s','factor':1},{'time':'25 s','factor':0}],'source':'default'})
  call('boundary_apply',{'name':'ambient_convection','kind':'convection','selection':'outer','convection':{'coefficient':40,'ambient':'20 degC'},'source':'default'})
  call('boundary_apply',{'name':'ambient_radiation','kind':'radiation','selection':'outer','radiation':{'emissivity':.7,'ambient':'20 degC'},'source':'default'})
  op='analysis_run';args={'analysis':'transient_thermal','end_time':'150 s','time_step':'1.25 s','theta':1,'capacity':'lumped','initial_temperature':'20 degC','output_times':[2.5*i for i in range(1,61)]}
  assert call('setup_validate',args)['ready']
 call('project_save',{});record['inputs']=args
 start=time.monotonic();job=call(op,args);record['run']=job;save()
 while True:
  st=call('job_status',{'job_id':job['job_id'],'wait_seconds':10});record['status']=st;save()
  if st['state'] not in ('queued','running'):break
  print(kind,st.get('progress'),st.get('stage'),round(time.monotonic()-start,1),flush=True)
  if time.monotonic()-start>480:
   call('job_cancel',{'job_id':job['job_id']});raise RuntimeError('480-second compute budget exceeded')
 record['elapsed_seconds']=time.monotonic()-start
 assert st['state']=='succeeded',json.dumps(st)
 s=st['summary'];r=s.get('results',{})
 if kind=='fdm':
  assert r['worst_heat_balance_relative']<=1e-6
  assert r['equilibrium_error_last_solve']<=1e-6
 elif kind=='lpbf': assert r['equilibrium_error_last_solve']<=1e-6
 else:
  e=s['energy'];assert e['closure_error']<=1e-6 and e['worst_step_balance_error']<=1e-6 and e['enthalpy_mismatch']<=1e-9
 path=Path(job['run_directory'])/'results.nvt'
 with path.open('rb') as f:
  assert f.read(8)==b'NVTHR001';h=json.loads(f.read(struct.unpack('<Q',f.read(8))[0]));record['result_header']=h
  finite={}
  for arr in h['arrays']:
   typ=arr['type'];size={'d':8,'i':4,'b':1,'c':1}[typ]
   data=f.read(size*arr['count']);assert len(data)==size*arr['count']
   if typ=='d':
    values=array.array('d');values.frombytes(data)
    if sys.byteorder!='little':values.byteswap()
    assert all(map(math.isfinite,values)),arr['name']
    if arr['name'] in ('T','mech_u','mech_vm','times'):finite[arr['name']]={'count':len(values),'min':min(values),'max':max(values)}
  record['finite_arrays']=finite
 record['results_sha256']=hashlib.sha256(path.read_bytes()).hexdigest();record['acceptance_passed']=True
 call('project_save',{});save()
 print(kind,'PASS',json.dumps({'elapsed_seconds':record['elapsed_seconds'],'results':r,'finite':record['finite_arrays']}),flush=True)
finally:c.close()
