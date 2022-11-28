const dictionary& alphaControls = mesh.solverDict(alpha1.name());

label nAlphaSubCycles(readScalar(alphaControls.lookup("nAlphaSubCycles")));
