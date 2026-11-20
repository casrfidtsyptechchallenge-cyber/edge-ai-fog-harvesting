"""Machine learning on the laptop (M5 models, trained and exported; M11 turns this into a pipeline).

Needs numpy (pip install -r requirements-ml.txt); nothing here runs on the Pi or on the nodes. What runs on
the nodes is C++ in firmware/node/lib/edge_ai, generated from here (arc_model_data.h).

    python -m ml arc-train        train the arc predictor (M5a) and write firmware/node/lib/edge_ai/arc_model_data.h
    python -m ml arc-eval         its accuracy on unseen runs, and arcs per hour in a closed loop

docs/edge_ai.md has the details.
"""