#!/usr/bin/env python3
"""
OTA Upload Script para CrySense AI v2.0
Usa espota.py (ferramenta Espressif nativa) para upload sem IDE
"""

import os
import sys
import socket
import subprocess
import argparse
from pathlib import Path
from datetime import datetime

# Cores para terminal
class Colors:
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    CYAN = '\033[96m'
    END = '\033[0m'

def print_status(msg, color=Colors.CYAN):
    """Imprime mensagem com timestamp"""
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"{color}[{ts}] {msg}{Colors.END}")

def print_error(msg):
    print_status(msg, Colors.RED)

def print_success(msg):
    print_status(msg, Colors.GREEN)

def print_warning(msg):
    print_status(msg, Colors.YELLOW)

def test_connectivity(ip_addr, port=3232):
    """Testa conectividade UDP ao ESP32"""
    print_status(f"Testando conectividade em {ip_addr}:{port}...")
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(2)
        # Envia pacote de teste
        sock.sendto(b"test", (ip_addr, port))
        sock.close()
        print_success("Conectividade OK!")
        return True
    except (socket.timeout, socket.error) as e:
        print_error(f"Falha na conectividade: {e}")
        print_warning("Verifique:")
        print_warning("  1. Se PC e ESP estão na mesma rede Wi-Fi")
        print_warning("  2. Se firewall permite UDP 3232")
        print_warning("  3. Se o IP está correto")
        return False

def find_binary(search_dir=None):
    """Procura arquivo .bin mais recente (Arduino IDE build)"""
    if search_dir is None:
        # Procura em pastas comuns do Arduino IDE Windows
        search_dirs = [
            Path(os.path.expandvars("%TEMP%")).glob("**/crySence.ino.esp32s3.bin"),
            Path(os.path.expandvars("%TEMP%")).glob("arduino_build_*/*.bin"),
            Path(os.path.expandvars("%TEMP%")).glob("arduino_build_*/crySence.ino.bin"),
        ]
        
        binaries = []
        for pattern in search_dirs:
            binaries.extend(pattern)
        
        if binaries:
            # Retorna o mais recente
            latest = max(binaries, key=lambda p: p.stat().st_mtime)
            return latest
    
    return None

def download_espota():
    """Download de espota.py se necessário"""
    espota_path = Path(__file__).parent / "espota.py"
    
    if espota_path.exists():
        print_status("espota.py já existe")
        return espota_path
    
    print_status("Baixando espota.py do repositório Espressif...")
    url = "https://raw.githubusercontent.com/espressif/arduino-esp32/master/tools/espota.py"
    
    try:
        import urllib.request
        urllib.request.urlretrieve(url, espota_path)
        os.chmod(espota_path, 0o755)
        print_success("espota.py baixado com sucesso")
        return espota_path
    except Exception as e:
        print_error(f"Falha ao baixar espota.py: {e}")
        return None

def perform_ota_upload(ip_addr, binary_file, password=None):
    """Executa upload OTA usando espota.py"""
    
    # Garante que espota.py existe
    espota_path = download_espota()
    if not espota_path:
        return False
    
    # Parâmetros para espota.py
    cmd = [
        sys.executable,
        str(espota_path),
        "-i", ip_addr,
        "-f", str(binary_file),
    ]
    
    if password:
        cmd.extend(["-a", password])
    
    print_status(f"Iniciando upload OTA para {ip_addr}...")
    print_status(f"Arquivo: {binary_file.name}")
    print_status(f"Tamanho: {binary_file.stat().st_size / 1024:.1f} KB")
    print_warning("Este processo pode levar 1-2 minutos...")
    
    try:
        result = subprocess.run(cmd, capture_output=False, text=True)
        if result.returncode == 0:
            print_success("Upload OTA concluído com sucesso!")
            print_status("ESP32 irá reiniciar automaticamente...")
            return True
        else:
            print_error(f"Upload falhou com código {result.returncode}")
            return False
    except Exception as e:
        print_error(f"Erro ao executar espota.py: {e}")
        return False

def main():
    parser = argparse.ArgumentParser(
        description="OTA Upload para CrySense AI v2.0",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Exemplos:
  python ota_upload.py 192.168.15.32
  python ota_upload.py 192.168.15.32 -b /caminho/para/firmware.bin
  python ota_upload.py 192.168.15.32 -p minha_senha
        """
    )
    
    parser.add_argument("ip", nargs="?", default=None,
                       help="IP do ESP32 (ex: 192.168.15.32)")
    parser.add_argument("-b", "--binary", type=Path, default=None,
                       help="Caminho do arquivo .bin (auto-detecta se não informado)")
    parser.add_argument("-p", "--password", type=str, default=None,
                       help="Senha OTA (se configurada)")
    
    args = parser.parse_args()
    
    # IP do ESP32
    if args.ip is None:
        print_warning("IP não informado!")
        ip_input = input("Digite o IP do ESP32: ").strip()
        if not ip_input:
            print_error("IP é obrigatório!")
            sys.exit(1)
        ip_addr = ip_input
    else:
        ip_addr = args.ip
    
    print_status(f"Alvo: {ip_addr}")
    
    # Arquivo binário
    if args.binary:
        binary_file = args.binary
        if not binary_file.exists():
            print_error(f"Arquivo não encontrado: {binary_file}")
            sys.exit(1)
    else:
        print_status("Procurando arquivo .bin mais recente...")
        binary_file = find_binary()
        if not binary_file:
            print_error("Nenhum arquivo .bin encontrado!")
            print_warning("Passos:")
            print_warning("  1. Compile o projeto no Arduino IDE (Sketch → Verificar ou Upload)")
            print_warning("  2. Na IDE, ative 'Arquivo → Preferências → Mostrar saída detalhada'")
            print_warning("  3. Procure pela linha 'Escrevendo em flash' para localizar o .bin")
            print_warning("  4. Execute: python ota_upload.py <IP> -b <caminho_do_bin>")
            sys.exit(1)
        print_success(f"Arquivo encontrado: {binary_file}")
    
    # Teste de conectividade
    if not test_connectivity(ip_addr):
        print_warning("Mesmo assim tentando upload (pode falhar)...")
    
    # Upload OTA
    if perform_ota_upload(ip_addr, binary_file, args.password):
        print_success("===== Upload Concluído =====")
        print_status("Próximos passos:")
        print_status(f"1. Aguarde ESP32 reiniciar (LED de inicialização)")
        print_status(f"2. Serial Monitor deve mostrar '[OTA] Servico de gravacao sem fio iniciado!'")
        print_status(f"3. Acesse http://{ip_addr}/api/status para confirmar funcionamento")
        sys.exit(0)
    else:
        print_error("Upload falhou!")
        sys.exit(1)

if __name__ == "__main__":
    main()
