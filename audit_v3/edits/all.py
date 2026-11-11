import runpy, sys, os
here = os.path.dirname(os.path.abspath(__file__))
for name in ('f3_01.py', 'f3_02.py', 'f3_03.py', 'f3_04.py', 'f3_04_doc.py', 'f3_05.py', 'f3_06.py'):
    runpy.run_path(os.path.join(here, name), run_name='__main__')
