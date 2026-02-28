{
  lib,
  buildPythonPackage,
  fetchFromGitHub,
  setuptools,
  pythonOlder,
  # Core dependencies
  torchdata,
  datasets,
  tokenizers,
  tomli,
  fsspec,
  tyro,
  tensorboard,
  # Optional dependencies
  pre-commit,
  pytest,
  pytest-cov,
  wandb,
  tomli-w,
  expecttest,
  # Optional nanosets
  datatrove ? null,
  numba ? null,
  # Optional transformers
  transformers ? null,
  # Feature flags
  withDev ? false,
  withNanosets ? false,
  withTransformers ? false,
}:

let
  src = fetchFromGitHub {
    owner = "NousResearch";
    repo = "torchtitan";
    rev = "a43ee472cee64dd3a771c5be9903ecd3e20f36a1";
    hash = "sha256-MLUmCW/ZR32cm3qsGOo4X1Q9VZle2rZmRiveVnWZsuU=";
  };
  version = lib.removeSuffix "\n" (builtins.readFile (src + "/assets/version.txt"));
in
buildPythonPackage {
  pname = "torchtitan";
  inherit src version;
  format = "pyproject";

  disabled = pythonOlder "3.10";

  nativeBuildInputs = [
    setuptools
  ];

  propagatedBuildInputs = [
    torchdata
    datasets
    tokenizers
    tomli
    fsspec
    tyro
    tensorboard
  ]
  ++ lib.optionals withDev [
    pre-commit
    pytest
    pytest-cov
    wandb
    tomli-w
    expecttest
  ]
  ++ lib.optionals withNanosets [
    datatrove
    numba
  ]
  ++ lib.optionals withTransformers [
    transformers
  ];

  nativeCheckInputs = [
    pytest
    pytest-cov
  ]
  ++ lib.optionals (!withDev) [
    tomli-w
    expecttest
  ];

  pythonImportsCheck = [
    "torchtitan"
  ];

  checkPhase = ''
    runHook preCheck
    pytest tests/
    runHook postCheck
  '';

  # Skip tests by default since they may require GPU
  doCheck = false;

  meta = with lib; {
    description = "A PyTorch native platform for training generative AI models";
    homepage = "https://github.com/NousResearch/torchtitan";
    platforms = platforms.unix;
  };
}
