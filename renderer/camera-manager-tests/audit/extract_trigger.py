from pathlib import Path
import sys
text=(Path(sys.argv[1])/'ryty_gpu/ryty_game_patch.cpp').read_text()
a=text.index('void InstallTriggerTelemetry(');b=text.index('{',a);c=b+1;depth=1
while depth:
    depth+=(text[c]=='{')-(text[c]=='}');c+=1
Path(sys.argv[2]).write_text(text[a:c]+'\n')
