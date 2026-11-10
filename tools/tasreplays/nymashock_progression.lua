-- Controller-only authoring/replay. No state loads, RAM writes or card injection.
local function session()
 local root,prefix=assert(ROUTE_ROOT),assert(ROUTE_PREFIX)
 assert(emu.framecount()==0,'cold source required')
 if prefix>0 then
  assert(movie.mode()=='PLAY' and movie.getreadonly() and movie.length()==227202)
  assert(not movie.startsfromsavestate() and not movie.startsfromsaveram())
 else assert(not movie.isloaded(),'fresh card load must not start a movie') end
 luanet.load_assembly('System')
 local Assembly=luanet.import_type('System.Reflection.Assembly')
 local assembly=Assembly.LoadFrom(assert(ROUTE_HELPER))
 luanet.load_assembly(assembly.FullName)
 local helper=assert(luanet.import_type('Progression291'))
 helper.Initialize(root,ROUTE_CORE_SHA,ROUTE_WBX_SHA,true,prefix>0,ROUTE_CARD_SHA)
 local ram=assert(io.open(root..'/ram-frames.tsv','w'))
 ram:write('frame\tcycle\tlag_count\tram_sha256\n')
 local input=assert(io.open(root..'/controller.tsv','w'))
 input:write('frame\tbuttons\tly\tlx\try\trx\tanalog\n')
 local names={'Select','Left Stick, Button','Right Stick, Button','Start','D-Pad Up','D-Pad Right','D-Pad Down','D-Pad Left','L2','R2','L1','R1','△','○','X','□'}
 local axisNames={'Left Stick Up / Down','Left Stick Left / Right','Right Stick Up / Down','Right Stick Left / Right'}
 local function state(buttons)
  local word=65535
  for i,name in ipairs(names) do
   local value=buttons['P1 '..name];assert(type(value)=='boolean','unknown controller shape')
   if value then word=word-2^(i-1) end
  end
  local axes={}
  for i,name in ipairs(axisNames) do
   local value=buttons['P1 '..name];assert(type(value)=='number' and value>=0 and value<=255 and value%1==0)
   axes[i]=value
  end
  assert(buttons['P1 Analog']==false,'physical Analog unsupported')
  for _,name in ipairs({'Power','Reset','Open Tray','Close Tray'}) do assert(buttons[name]==false,'console event unsupported') end
  assert(buttons['Disk Index']==0,'disc event unsupported')
  return word,axes
 end
 local function recordInput(buttons)
  local word,axes=state(buttons)
  input:write(string.format('%d\t%d\t%d\t%d\t%d\t%d\t0\n',emu.framecount()+1,word,axes[1],axes[2],axes[3],axes[4]))
 end
 local function observe()
  local frame=emu.framecount();assert(frame<=300000,'session return limit')
  local hash=helper.Record(frame)
  ram:write(string.format('%d\t%d\t%d\t%s\n',frame,helper.Clock(),emu.lagcount(),hash:upper()))
  if frame%300==0 then
   ram:flush();input:flush()
   local f=assert(io.open(root..'/progress.json','w'));f:write(string.format('{"frame":%d,"clock":%d}\n',frame,helper.Clock()));f:close()
  end
  if frame%1200==0 then client.screenshot(root..string.format('/frame-%06d.png',frame)) end
 end
 observe();client.unpause()
 while emu.framecount()<prefix do
  recordInput(movie.getinput(emu.framecount()))
  emu.frameadvance();assert(movie.getreadonly());observe()
 end
 if prefix>0 then movie.stop(false);assert(movie.mode()=='INACTIVE') end
 local function capture(step)
  client.pause();ram:flush();input:flush();helper.Capture(step)
  client.screenshot(root..string.format('/step-%06d.png',step))
  local f=assert(io.open(root..'/ready.json','w'));f:write(string.format('{"completed_step":%d,"frame":%d}\n',step,emu.framecount()));f:close()
 end
 capture(0)
 for step=1,4096 do
  local path=root..string.format('/commands/%06d.txt',step)
  local command=io.open(path,'r')
  while not command do emu.yield();command=io.open(path,'r') end
  local text=command:read('*a');command:close();assert(#text<=128,'oversized controller command')
  if text=='finish\n' then
   helper.Finish();ram:close();input:close()
   local f=assert(io.open(root..'/complete.json','w'));f:write(string.format('{"frame":%d,"completed_steps":%d,"original_prefix":%d,"authored_route":true}\n',emu.framecount(),step-1,prefix));f:close()
   client.exitCode(0);return
  end
  local values={}
  assert(text:match('^%d+ %d+ %d+ %d+ %d+ %d+\n$'),'malformed controller command')
  for number in text:gmatch('%d+') do table.insert(values,tonumber(number)) end
  assert(#values==6 and values[1]>=1 and values[1]<=12000 and values[2]<=65535 and emu.framecount()+values[1]<=300000)
  local buttons,axes={},{}
  for key,value in pairs(joypad.get()) do
   if type(value)=='boolean' then buttons[key]=false
   elseif key=='Disk Index' then axes[key]=0 end
  end
  for i,name in ipairs(names) do buttons['P1 '..name]=math.floor(values[2]/2^(i-1))%2==0 end
  for i,name in ipairs(axisNames) do assert(values[i+2]<=255);axes['P1 '..name]=values[i+2] end
  client.unpause()
  for _=1,values[1] do
   assert(movie.mode()=='INACTIVE','authored route must not record a movie')
   joypad.set(buttons);joypad.setanalog(axes)
   local actual=joypad.get();local word,seen=state(actual)
   assert(word==values[2]);for i=1,4 do assert(seen[i]==values[i+2]) end
   recordInput(actual);emu.frameadvance();observe()
  end
  capture(step)
 end
 error('session step limit')
end
local okay,errorMessage=xpcall(session,debug.traceback)
if not okay then
 local f=io.open(assert(ROUTE_ROOT)..'/route-error.txt','w')
 if f then f:write(tostring(errorMessage));f:close() end
 client.exitCode(1)
end
