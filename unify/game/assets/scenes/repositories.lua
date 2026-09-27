-- Read-only presentation of the C# WorldManifest. Generated data never writes back to the core.
local buildings = {{district="\105\110\102\114\097\115\116\114\117\099\116\117\114\101",category="\115\121\115\116\101\109\115\073\110\102\114\097\115\116\114\117\099\116\117\114\101",activity="\108\111\119",construction="\117\110\114\101\108\101\097\115\101\100",repo="\083\078\065\080\075\073\084\084\089\087\069\083\084\047\114\101\112\111\118\101\114\115\101",seed="\051\052\057\099\055\055\052\098\053\099\100\098\050\054\051\051",floors=2,rooms=16}}
local S = {}
function S:init()
 input:bind("next", "right", "d")
 input:bind("prev", "left", "a")
 input:bind("play", "enter")
 input:bind("back", "escape")
end
function S:load() self.state={selected=1}; camera:set(0,0,32) end
function S:update(dt)
 if input:is_pressed("next") and #buildings>0 then self.state.selected=self.state.selected % #buildings + 1 end
 if input:is_pressed("prev") and #buildings>0 then self.state.selected=(self.state.selected-2) % #buildings + 1 end
 if input:is_pressed("back") then scene.transition("snapkitty") end
 if input:is_pressed("play") then scene.transition("snapkitty_level") end
end
function S:render()
 local w,h=ui:screen()
 ui:image("textures/snapkitty/snap-os",0,0,w,h)
 ui:rect(0,0,w,h,5,9,20,230)
 ui:text("REPOVERSE / REPOSITORIES",24,22,2.3,120,230,250)
 local b=buildings[self.state.selected]
 if b then
  ui:text(b.repo:sub(1,48),24,70,1.8,230,220,255)
  ui:text("DISTRICT "..b.district.." / "..b.category,24,112,1.4,200,210,230)
  ui:text("ACTIVITY "..b.activity.." / "..b.construction,24,141,1.3,180,220,200)
  ui:text(b.floors.." FLOORS / "..b.rooms.." ROOMS",24,171,1.5,240,210,145)
  ui:text("SEED "..b.seed,24,204,1.2,165,185,205)
  ui:text(self.state.selected.." / "..#buildings.."  -  LEFT / RIGHT TO BROWSE",24,254,1.3,195,210,240)
 else
  ui:text("NO WORLD MANIFEST IMPORTED",24,94,2,230,210,150)
  ui:text("RUN tools/export-unify-world.py WITH A MANIFEST",24,139,1.2,210,220,240)
 end
 ui:text("ENTER PLAY BIFROST RUNNER   ESC MENU",24,h-35,1.3,210,230,250)
end
return S
