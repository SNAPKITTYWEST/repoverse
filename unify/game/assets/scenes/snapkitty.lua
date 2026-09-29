-- SNAPKITTY frontend. Uses the same Lua/render API on SDL, headless and Unreal.
local S = {}
function S:init()
 input:bind("start", "enter", "space", "pad_start", "pad_a")
 input:bind("continue", "f9")
 input:bind("info", "i")
 input:bind("repos", "r")
 input:bind("quit", "escape")
end
function S:load()
 self.state = {time=0, info=false}
 camera:set(0,0,32)
 audio:stream("music/theme",0.35,true,1)
end
function S:update(dt)
 self.state.time=self.state.time+dt
 if input:is_pressed("start") then scene.transition("snapkitty_level") end
 if input:is_pressed("continue") then load_state("slot1") end
 if input:is_pressed("repos") then scene.transition("repositories") end
 if input:is_pressed("info") then self.state.info=not self.state.info end
 if input:is_pressed("quit") then engine.quit() end
end
function S:on_loaded(ok)
 self.state.load_error = not ok
end
function S:render()
 local w,h=ui:screen()
 ui:image("textures/snapkitty/civilization",0,0,w,h)
 ui:rect(0,0,w,h,5,9,20,195)
 ui:image("textures/snapkitty/kitten",w-230,55,230,230,255,255,255,255,1000)
 ui:text("REPOVERSE",26,25,3,80,225,240)
 ui:text("SNAPKITTY",26,72,5,245,240,255)
 ui:text("BIFROST RUNNER",28,116,2,185,160,255)
 ui:text("POWERED BY UNIFY",28,148,1.5,190,205,225)
 ui:rect(24,184,345,44,70,45,140,240)
 ui:text("ENTER / SPACE  -  PLAY",36,200,2,255,255,255)
 ui:text("A/D MOVE   SPACE JUMP   E TALK",28,248,1.4,220,230,245)
 ui:text("F5 SAVE   F9 LOAD   ESC MENU",28,273,1.4,220,230,245)
 ui:text("F9 CONTINUE   R REPOS   I ABOUT   ESC EXIT",28,h-28,1.2,160,180,210)
 if self.state.load_error then ui:text("NO SAVED GAME - ENTER TO START",28,302,1.3,255,210,150) end
 if self.state.info then
  ui:rect(20,174,w-40,150,6,12,28,250)
  ui:text("COLLECT EVENTS. REACH THE TERMINAL.",32,193,1.5,130,235,250)
  ui:text("THE ARCHITECT IS WAITING NEAR THE START.",32,220,1.3,225,225,245)
  ui:text("SAME GAMEPLAY IN UNIFY AND THE UNREAL HOST.",32,247,1.2,190,205,230)
  ui:text("ART: SNAPKITTY COLLECTIVE / SEE ASSET LICENSE",32,277,1,170,180,205)
 end
end
return S
