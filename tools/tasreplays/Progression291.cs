// Read-only progression observer for the separately licensed stock Nymashock2.9.1 host.
// Controller input belongs to the external Lua route. No core patch or state/memory write.
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Windows.Forms;
using BizHawk.Common;
using BizHawk.Client.EmuHawk;
using BizHawk.Client.Common;
using BizHawk.Emulation.Common;
using BizHawk.Emulation.Cores.Sony.PSX;
using BizHawk.Emulation.Cores.Waterbox;
using Newtonsoft.Json;
public static class Progression291 {
 static MainForm form;static Nymashock core;static IMemoryDomains domains;
 static string root;static StreamWriter pages;static int previous=-1;
 static readonly byte[] ram=new byte[2097152];static readonly SHA256 sha=SHA256.Create();
 static string Hex(byte[] b){return BitConverter.ToString(b).Replace("-","").ToLowerInvariant();}
 static string FileHash(string path){using(var f=File.OpenRead(path))return Hex(sha.ComputeHash(f));}
 static byte[] ReadDomain(MemoryDomain domain){
  if(domain.Size<0 || domain.Size>2097152)throw new InvalidOperationException("unqualified domain size");
  var result=new byte[(int)domain.Size];
  using(domain.EnterExit())domain.BulkPeekByte(0L.RangeTo(domain.Size-1),result);
  return result;
 }
 static void JsonFile(string name,object data){File.WriteAllText(Path.Combine(root,name),JsonConvert.SerializeObject(data,Formatting.Indented));}
 public static long Clock(){if(core==null)throw new InvalidOperationException("not initialized");return core.CycleCount;}
 public static void Initialize(string directory,string expectedCoreAssembly,string expectedWaterbox,bool withPages,bool originalPrefix,string expectedCard){
  if(root!=null)throw new InvalidOperationException("observer already initialized");
  foreach(Form candidate in Application.OpenForms){var found=candidate as MainForm;if(found!=null){if(form!=null)throw new InvalidOperationException("multiple main forms");form=found;}}
  if(form==null || !(form.Emulator is Nymashock))throw new InvalidOperationException("exact Nymashock host required");
  core=(Nymashock)form.Emulator;root=directory;
  if(core.Frame!=0 || core.CycleCount!=0 || (originalPrefix && !form.MovieSession.ReadOnly))throw new InvalidOperationException("cold frame-zero source required");
  string assembly=typeof(Nymashock).Assembly.Location,wbx=Path.Combine(Application.StartupPath,"dll","shock.wbx.zst");
  if(FileHash(assembly)!=expectedCoreAssembly || FileHash(wbx)!=expectedWaterbox)throw new InvalidOperationException("core file binding mismatch");
  domains=(IMemoryDomains)core.ServiceProvider.GetService(typeof(IMemoryDomains));
  if(domains["MainRAM"].Size!=ram.Length || domains["BiosROM"].Size!=524288)throw new InvalidOperationException("wrong source memory domains");
  byte[] bios=ReadDomain(domains["BiosROM"]);
  if(Hex(sha.ComputeHash(bios))!="9c0421858e217805f4abe18698afea8d5aa36ff0727eb8484944e00eb5e7eadb")throw new InvalidOperationException("wrong loaded Japanese BIOS");
  JsonFile("loaded-bios.json",new {sha256=Hex(sha.ComputeHash(bios)),bytes=bios.Length});
  JsonFile("loaded-core.json",new {assembly=assembly,assembly_sha256=FileHash(assembly),waterbox=wbx,waterbox_sha256=FileHash(wbx),type=core.GetType().FullName,clock="public WaterboxCore.CycleCount"});
  var sync=core.GetSyncSettings();JsonFile("effective-sync.json",sync);JsonFile("effective-settings.json",core.GetSettings());
  var query=typeof(NymaCore).GetMethod("SettingsQuery",BindingFlags.Instance|BindingFlags.NonPublic,null,new Type[]{typeof(string)},null);
  if(query==null)throw new InvalidOperationException("source effective setting getter missing");
  var settings=new System.Collections.Generic.SortedDictionary<string,string>();
  foreach(var setting in core.SettingsInfo.AllSettings)settings.Add(setting.SettingsKey,(string)query.Invoke(core,new object[]{setting.SettingsKey}));
  JsonFile("effective-setting-values.json",settings);
  var ports=new System.Collections.Generic.List<object>();
  for(int i=0;i<core.SettingsInfo.Ports.Count;i++){
   string device;bool explicitValue=sync.PortDevices.TryGetValue(i,out device);if(!explicitValue)device=core.SettingsInfo.Ports[i].DefaultSettingsValue;
   ports.Add(new {index=i,name=core.SettingsInfo.Ports[i].Name,device=device,explicit_value=explicitValue});
   if(device!=(i==0?"dualshock":"none"))throw new InvalidOperationException("unexpected effective controller");
  }
  JsonFile("effective-ports.json",ports);
  for(int i=1;i<=8;i++)if(settings["psx.input.port"+i+".memcard"]!=(i==1?"1":"0"))throw new InvalidOperationException("unexpected effective memory-card setting");
  var cards=new System.Collections.Generic.List<object>();
  if(domains["Memcard 1"].Size!=131072 || Hex(sha.ComputeHash(ReadDomain(domains["Memcard 1"])))!=expectedCard)throw new InvalidOperationException("actual loaded card differs");
  foreach(MemoryDomain d in domains){if(!d.Name.StartsWith("Memcard "))continue;byte[] data=ReadDomain(d);string name="initial-"+d.Name.Replace(' ','-')+".bin";File.WriteAllBytes(Path.Combine(root,name),data);cards.Add(new {domain=d.Name,bytes=data.Length,sha256=Hex(sha.ComputeHash(data)),file=name});}
  JsonFile("initial-cards.json",cards);
  var config=(Config)typeof(MainForm).GetProperty("Config",BindingFlags.Instance|BindingFlags.NonPublic).GetValue(form,null);
  JsonFile("effective-host-config.json",config);
  string savePath=Path.GetFullPath(config.PathEntries.AbsolutePathForType("PSX","Save RAM"));
  string expectedPath=Path.GetFullPath(Path.Combine(root,"SaveRAM"));
  if(!String.Equals(savePath.TrimEnd('\\','/'),expectedPath.TrimEnd('\\','/'),StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("source card path is not isolated");
  if(config.Movies.MovieEndAction!=MovieEndAction.Finish)throw new InvalidOperationException("movie end policy must be Finish");
  JsonFile("effective-run-policy.json",new {read_only=form.MovieSession.ReadOnly,movie_end_action=config.Movies.MovieEndAction.ToString(),save_ram_directory=savePath});
  if(withPages){pages=new StreamWriter(new FileStream(Path.Combine(root,"ram-pages.tsv"),FileMode.CreateNew));pages.Write("# psx-ram-pages-v1 page_bytes=4096 ram_bytes=2097152 hash=fnv1a64\nframe\tcycle");for(int i=0;i<512;i++)pages.Write("\t"+(i*4096).ToString("X6"));pages.WriteLine();}
 }
 public static string Record(int frame){
  if(core==null || frame!=previous+1 || frame!=core.Frame || frame>1000000)throw new InvalidOperationException("invalid source return sequence");
  var domain=domains["MainRAM"];using(domain.EnterExit())domain.BulkPeekByte(0L.RangeTo((long)ram.Length-1),ram);
  if(pages!=null && frame>0){pages.Write(frame+"\t"+Clock());for(int page=0;page<512;page++){ulong h=14695981039346656037UL;unchecked{for(int i=page*4096;i<(page+1)*4096;i++)h=(h^ram[i])*1099511628211UL;}pages.Write("\t"+h.ToString("X16"));}pages.WriteLine();if(frame%60==0)pages.Flush();}
  previous=frame;return Hex(sha.ComputeHash(ram));
 }
 public static void Capture(int step){
  if(core==null || step<0 || step>4096 || core.Frame!=previous)throw new InvalidOperationException("invalid capture boundary");
  string stem="step-"+step.ToString("D6");
  var card=ReadDomain(domains["Memcard 1"]);
  string cardFile=stem+"-card1.mcd",ramFile=stem+"-ram.bin";
  if(File.Exists(Path.Combine(root,cardFile)) || File.Exists(Path.Combine(root,ramFile)))throw new InvalidOperationException("never overwrite capture");
  File.WriteAllBytes(Path.Combine(root,cardFile),card);File.WriteAllBytes(Path.Combine(root,ramFile),ram);
  JsonFile(stem+".json",new {step=step,frame=core.Frame,clock=Clock(),card1_file=cardFile,card1_sha256=Hex(sha.ComputeHash(card)),ram_file=ramFile,ram_sha256=Hex(sha.ComputeHash(ram)),scope="read-only source boundary; semantic review and fresh reload required"});
 }
 public static void Finish(){if(pages!=null){pages.Close();pages=null;}if(previous>=0)File.WriteAllBytes(Path.Combine(root,"ram-frame-"+previous.ToString("D6")+".bin"),ram);}
}
